#include "MfVideoEncoder.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <cstring>
#include <algorithm>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct MfVideoEncoder::Impl {
    IMFTransform* mft = nullptr;
    IMFMediaType* inputType = nullptr;
    IMFMediaType* outputType = nullptr;

    // D3D11 interop for hardware GPU encoding
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IMFDXGIDeviceManager* deviceManager = nullptr;
    UINT deviceResetToken = 0;

    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t bitrate = 0;             // 0 = auto
    uint32_t fps = 30;
    uint32_t frameIndex = 0;
    uint32_t codecType = 0;           // 0=H.264, 1=HEVC

    bool initialized = false;
    bool needKeyFrame = true;

    // Codec data (SPS/PPS) read from output type after configuration
    // Prepend to first frame after each keyframe request
    std::vector<uint8_t> codecData;
};

MfVideoEncoder::MfVideoEncoder() : m_impl(std::make_unique<Impl>()) {}

MfVideoEncoder::~MfVideoEncoder() { Shutdown(); }

// Activate one encoder candidate and try to set up D3D11 interop.
// On success m_impl->mft is ready for ConfigureMediaTypes().
// Returns false if activation failed (skip to next candidate).
bool MfVideoEncoder::SetupEncoderCandidate(
    IMFActivate* activate, unsigned int idx,
    ID3D11Device* captureDevice, ID3D11DeviceContext* captureContext,
    MfVideoEncoder::Impl* impl) {

    HRESULT hr = E_FAIL;
    __try {
        hr = activate->ActivateObject(IID_PPV_ARGS(&impl->mft));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("  Encoder #%u: ActivateObject crashed (exception 0x%08X)",
                    idx, GetExceptionCode());
        hr = E_FAIL;
    }
    if (FAILED(hr)) return false;

    if (!captureDevice) return true; // no D3D available, CPU-only

    // Check if this MFT supports D3D11. Only attempt D3D manager setup
    // for MFTs that explicitly advertise D3D11 awareness; sending
    // MFT_MESSAGE_SET_D3D_MANAGER to a non-D3D-aware MFT may crash.
    {
        UINT32 d3dVal = 0;
        if (FAILED(activate->GetUINT32(MF_SA_D3D11_AWARE, &d3dVal)) || d3dVal == 0) {
            return true; // software MFT or attribute missing, CPU-only
        }
    }

    // Try the pre-existing device manager (from Initialize) first.
    // If the MFT accepts it, we can share GPU textures with the capture pipeline.
    bool accepted = false;
    if (impl->deviceManager) {
        if (SUCCEEDED(impl->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                 (ULONG_PTR)impl->deviceManager))) {
            accepted = true;
        } else {
            // MFT rejected the capture device's manager — clear it and probe
            impl->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            impl->deviceManager->Release();
            impl->deviceManager = nullptr;
        }
    }

    // If the pre-existing manager wasn't accepted, create a fresh one from the
    // capture device and try again (some MFTs need a freshly-created manager).
    if (!accepted && captureDevice) {
        IMFDXGIDeviceManager* dm = nullptr;
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &dm))) {
            if (SUCCEEDED(dm->ResetDevice(captureDevice, token))) {
                if (SUCCEEDED(impl->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                         (ULONG_PTR)dm))) {
                    impl->d3dDevice = captureDevice;
                    impl->d3dContext = captureContext;
                    impl->deviceManager = dm;
                    impl->deviceResetToken = token;
                    accepted = true;
                }
            }
            if (!accepted) {
                dm->Release();
                impl->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            }
        }
    }

    // If capture device wasn't accepted, probe all adapters
    if (!accepted) {
        static const UINT kProbeFlagSets[] = {
            0,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            D3D11_CREATE_DEVICE_SINGLETHREADED,
        };

        IDXGIFactory1* dxgiFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                          (void**)&dxgiFactory))) {
            for (UINT fi = 0; fi < ARRAYSIZE(kProbeFlagSets) && !accepted; ++fi) {
                IDXGIAdapter1* probeAdap = nullptr;
                for (UINT j = 0;
                     dxgiFactory->EnumAdapters1(j, &probeAdap) != DXGI_ERROR_NOT_FOUND;
                     ++j) {
                    ID3D11Device* probeDevice = nullptr;
                    ID3D11DeviceContext* probeCtx = nullptr;
                    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
                    if (SUCCEEDED(D3D11CreateDevice(probeAdap, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                                     kProbeFlagSets[fi], fls, ARRAYSIZE(fls),
                                                     D3D11_SDK_VERSION, &probeDevice, nullptr, &probeCtx))) {
                        IMFDXGIDeviceManager* probeMgr = nullptr;
                        UINT probeToken = 0;
                        if (SUCCEEDED(MFCreateDXGIDeviceManager(&probeToken, &probeMgr))) {
                            if (SUCCEEDED(probeMgr->ResetDevice(probeDevice, probeToken))) {
                                if (SUCCEEDED(impl->mft->ProcessMessage(
                                    MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)probeMgr))) {
                                    if (impl->deviceManager) impl->deviceManager->Release();
                                    impl->d3dDevice = probeDevice;
                                    impl->d3dContext = probeCtx;
                                    impl->deviceManager = probeMgr;
                                    impl->deviceResetToken = probeToken;
                                    accepted = true;
                                    probeAdap->Release();
                                    LOG_INFO("  Encoder #%u matched adapter #%u (flags=0x%X)",
                                             idx, j, kProbeFlagSets[fi]);
                                    break;
                                }
                                probeMgr->Release();
                            }
                            probeMgr->Release();
                        }
                        probeCtx->Release();
                        probeDevice->Release();
                    }
                    probeAdap->Release();
                }
            }
            dxgiFactory->Release();
        }

        if (!accepted) {
            // Use CPU input — encoder still accelerates encode internally
            LOG_INFO("  Encoder #%u using CPU input (D3D manager not accepted)", idx);
        }
    }

    return true;
}

bool MfVideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps,
                                ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
    m_impl->width = width;
    m_impl->height = height;
    m_impl->bitrate = bitrate;
    m_impl->fps = fps;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: 0x%08X", hr);
        return false;
    }

    // Create the D3D device manager before enumerating MFTs.
    // On some systems (e.g. Optimus laptops) this initialization is required
    // before hardware MFT activation, otherwise ActivateObject may crash.
    if (d3dDevice) {
        m_impl->d3dDevice = d3dDevice;
        m_impl->d3dContext = d3dContext;
        HRESULT hrDm = MFCreateDXGIDeviceManager(&m_impl->deviceResetToken,
                                                  &m_impl->deviceManager);
        if (SUCCEEDED(hrDm)) {
            hrDm = m_impl->deviceManager->ResetDevice(d3dDevice, m_impl->deviceResetToken);
            if (SUCCEEDED(hrDm)) {
                LOG_INFO("D3D11 device manager created from capture device");
            } else {
                m_impl->deviceManager->Release();
                m_impl->deviceManager = nullptr;
            }
        }
    }

    // --- For resolutions above 1080p, try HEVC first ---
    // The Microsoft H.264 software encoder silently caps output at 1920x1080
    // even when it accepts higher input/output media types. HEVC encoders do
    // not have this limitation, so we try them first for high resolutions.
    MFT_REGISTER_TYPE_INFO h264Input  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO h264Output = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO hevcInput  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO hevcOutput = { MFMediaType_Video, MFVideoFormat_HEVC };

    bool configured = false;

    if (width > 1920 || height > 1080) {
        LOG_INFO("Target resolution %ux%u > 1080p, trying HEVC encoders first...",
                 width, height);

        // --- Try HEVC hardware encoders ---
        IMFActivate** hevcHwActivates = nullptr;
        UINT32 hevcHwCount = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                                 &hevcInput, &hevcOutput,
                                 &hevcHwActivates, &hevcHwCount)) && hevcHwCount > 0) {
            for (UINT32 i = 0; i < hevcHwCount && !configured; ++i) {
                WCHAR* name = nullptr;
                UINT32 nameLen = 0;
                if (SUCCEEDED(hevcHwActivates[i]->GetAllocatedString(
                        MFT_FRIENDLY_NAME_Attribute, &name, &nameLen))) {
                    LOG_INFO("  HEVC encoder candidate #%u: %S", i, name);
                }

                if (m_impl->mft) { m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); m_impl->mft->Release(); m_impl->mft = nullptr; }
                if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
                if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
                if (m_impl->deviceManager) { m_impl->deviceManager->Release(); m_impl->deviceManager = nullptr; }

                if (!SetupEncoderCandidate(hevcHwActivates[i], i, d3dDevice, d3dContext, m_impl.get())) {
                    if (name) CoTaskMemFree(name);
                    continue;
                }
                m_impl->codecType = 1;
                if (ConfigureMediaTypes()) {
                    configured = true;
                    LOG_INFO("HEVC hardware encoder configured: %S",
                             name ? name : L"(unknown)");
                } else {
                    m_impl->codecType = 0;
                }
                if (name) CoTaskMemFree(name);
            }
            for (UINT32 i = 0; i < hevcHwCount; ++i) hevcHwActivates[i]->Release();
            CoTaskMemFree(hevcHwActivates);
        } else {
            LOG_INFO("No HEVC hardware encoders found");
        }

        // --- Try HEVC software encoders ---
        if (!configured) {
            LOG_INFO("Trying HEVC software encoders...");

            if (m_impl->mft) { m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); m_impl->mft->Release(); m_impl->mft = nullptr; }
            if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
            if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
            if (m_impl->deviceManager) { m_impl->deviceManager->Release(); m_impl->deviceManager = nullptr; }

            IMFActivate** hevcSwActivates = nullptr;
            UINT32 hevcSwCount = 0;
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                     MFT_ENUM_FLAG_SYNCMFT,
                                     &hevcInput, &hevcOutput,
                                     &hevcSwActivates, &hevcSwCount)) && hevcSwCount > 0) {
                UINT32 swIdx = 0;
                for (UINT32 i = 0; i < hevcSwCount; ++i) {
                    UINT32 d3d = 0;
                    if (FAILED(hevcSwActivates[i]->GetUINT32(MF_SA_D3D11_AWARE, &d3d)) || d3d == 0) {
                        if (swIdx != i) hevcSwActivates[swIdx] = hevcSwActivates[i];
                        ++swIdx;
                    } else { hevcSwActivates[i]->Release(); }
                }
                hevcSwCount = swIdx;
                LOG_INFO("Found %u HEVC software encoder candidates", hevcSwCount);

                for (UINT32 i = 0; i < hevcSwCount && !configured; ++i) {
                    if (FAILED(hevcSwActivates[i]->ActivateObject(IID_PPV_ARGS(&m_impl->mft))))
                        continue;
                    m_impl->codecType = 1;
                    if (ConfigureMediaTypes()) {
                        configured = true;
                        LOG_INFO("HEVC software encoder configured: %ux%u",
                                 m_impl->width, m_impl->height);
                    } else {
                        m_impl->mft->Release(); m_impl->mft = nullptr;
                        m_impl->codecType = 0;
                    }
                }
                for (UINT32 i = 0; i < hevcSwCount; ++i) hevcSwActivates[i]->Release();
                CoTaskMemFree(hevcSwActivates);
            } else {
                LOG_INFO("No HEVC software encoders found");
            }
        }
    }

    // --- Try H.264 hardware encoders if HEVC didn't work ---
    if (!configured) {
        LOG_INFO("Trying H.264 hardware encoders...");

        if (m_impl->mft) { m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); m_impl->mft->Release(); m_impl->mft = nullptr; }
        if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
        if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
        if (m_impl->deviceManager) { m_impl->deviceManager->Release(); m_impl->deviceManager = nullptr; }
        m_impl->codecType = 0;

        IMFActivate** hwActivates = nullptr;
        UINT32 hwCount = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                                 &h264Input, &h264Output,
                                 &hwActivates, &hwCount)) && hwCount > 0) {
            for (UINT32 i = 0; i < hwCount && !configured; ++i) {
                WCHAR* name = nullptr;
                UINT32 nameLen = 0;
                if (SUCCEEDED(hwActivates[i]->GetAllocatedString(
                        MFT_FRIENDLY_NAME_Attribute, &name, &nameLen))) {
                    LOG_INFO("  H.264 encoder candidate #%u: %S", i, name);
                }

                if (name) {
                    bool skip = (wcsstr(name, L"Intel") != nullptr ||
                                 wcsstr(name, L"Quick Sync") != nullptr ||
                                 wcsstr(name, L"NVIDIA") != nullptr);
                    if (skip) {
                        LOG_INFO("  Skipping H.264 #%u (known crash)", i);
                        CoTaskMemFree(name);
                        continue;
                    }
                }

                if (m_impl->mft) { m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); m_impl->mft->Release(); m_impl->mft = nullptr; }
                if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
                if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
                if (m_impl->deviceManager) { m_impl->deviceManager->Release(); m_impl->deviceManager = nullptr; }

                if (!SetupEncoderCandidate(hwActivates[i], i, d3dDevice, d3dContext, m_impl.get())) {
                    if (name) CoTaskMemFree(name);
                    continue;
                }
                if (ConfigureMediaTypes()) {
                    configured = true;
                    LOG_INFO("H.264 hardware encoder configured: %S",
                             name ? name : L"(unknown)");
                }
                if (name) CoTaskMemFree(name);
            }
            for (UINT32 i = 0; i < hwCount; ++i) hwActivates[i]->Release();
            CoTaskMemFree(hwActivates);
        }
    }

    // --- Fall back to software H.264 ---
    if (!configured) {
        m_impl->codecType = 0;
        LOG_WARNING("No hardware/HEVC encoder accepted %ux%u, falling back to software H.264",
                    width, height);

        if (m_impl->mft) { m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); m_impl->mft->Release(); m_impl->mft = nullptr; }
        if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
        if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
        if (m_impl->deviceManager) { m_impl->deviceManager->Release(); m_impl->deviceManager = nullptr; }

        IMFActivate** swActivates = nullptr;
        UINT32 swCount = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                             MFT_ENUM_FLAG_SYNCMFT,
                             &h264Input, &h264Output,
                             &swActivates, &swCount)) || swCount == 0) {
            LOG_ERROR("No software H.264 encoder available");
            return false;
        }

        UINT32 swIdx = 0;
        for (UINT32 i = 0; i < swCount; ++i) {
            UINT32 d3dAware = 0;
            if (FAILED(swActivates[i]->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) || d3dAware == 0) {
                if (swIdx != i) swActivates[swIdx] = swActivates[i];
                ++swIdx;
            } else { swActivates[i]->Release(); }
        }
        swCount = swIdx;

        if (swCount == 0) {
            CoTaskMemFree(swActivates);
            LOG_ERROR("No software H.264 encoder available after filtering");
            return false;
        }

        for (UINT32 i = 0; i < swCount && !configured; ++i) {
            if (FAILED(swActivates[i]->ActivateObject(IID_PPV_ARGS(&m_impl->mft))))
                continue;
            if (ConfigureMediaTypes()) {
                configured = true;
                LOG_INFO("Software H.264 encoder configured: %ux%u",
                         m_impl->width, m_impl->height);
            } else {
                m_impl->mft->Release(); m_impl->mft = nullptr;
            }
        }
        for (UINT32 i = 0; i < swCount; ++i) swActivates[i]->Release();
        CoTaskMemFree(swActivates);

        if (!configured) {
            LOG_ERROR("No encoder accepted the configuration");
            return false;
        }
    }

    m_impl->initialized = true;
    return true;
}

void MfVideoEncoder::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->mft) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->mft->Release();
    }
    if (m_impl->inputType) m_impl->inputType->Release();
    if (m_impl->outputType) m_impl->outputType->Release();
    if (m_impl->deviceManager) m_impl->deviceManager->Release();
    m_impl->mft = nullptr;
    m_impl->inputType = nullptr;
    m_impl->outputType = nullptr;
    m_impl->deviceManager = nullptr;
    m_impl->d3dDevice = nullptr;
    m_impl->d3dContext = nullptr;
}

// Compute a minimum reasonable bitrate for the given resolution and fps.
// Uses 0.1 bits-per-pixel as a floor — below this, H.264 encoders tend to
// silently downscale, defeating the configured resolution.
static uint32_t ComputeMinBitrate(uint32_t width, uint32_t height, uint32_t fps) {
    uint64_t pixels = static_cast<uint64_t>(width) * height;
    uint32_t minBps = static_cast<uint32_t>(pixels * fps / 10);
    if (minBps < 1000000) minBps = 1000000;
    return minBps;
}

// Build a resolution ladder from the target resolution, sorted descending.
// Each rung is tried as a complete output+input pair so we never end up with
// a resolution that SetOutputType accepts but SetInputType rejects.
static std::vector<std::pair<uint32_t, uint32_t>> BuildResolutionLadder(
    uint32_t targetWidth, uint32_t targetHeight) {

    // Common resolution rungs (descending by pixel count). Keep both 16:9 and
    // 16:10 variants so the ladder works for either native aspect ratio.
    static const std::pair<uint32_t, uint32_t> kRungs[] = {
        {3840, 2160}, {3840, 2400},
        {3440, 1440},
        {2560, 1600}, {2560, 1440}, {2560, 1080},
        {2048, 1280},
        {1920, 1200}, {1920, 1080},
        {1680, 1050},
        {1600,  900},
        {1440,  900},
        {1366,  768},
        {1280,  800}, {1280,  720},
    };

    std::vector<std::pair<uint32_t, uint32_t>> ladder;

    // Always include the native target
    ladder.push_back({targetWidth, targetHeight});

    // Add rungs not exceeding target dimensions (skip duplicates)
    for (auto& r : kRungs) {
        if (r.first <= targetWidth && r.second <= targetHeight) {
            if (r.first != targetWidth || r.second != targetHeight)
                ladder.push_back(r);
        }
    }

    // Sort descending by total pixels
    std::sort(ladder.begin(), ladder.end(),
        [](const auto& a, const auto& b) {
            uint64_t pa = static_cast<uint64_t>(a.first) * a.second;
            uint64_t pb = static_cast<uint64_t>(b.first) * b.second;
            if (pa != pb) return pa > pb;
            return a.second > b.second;
        });

    return ladder;
}

bool MfVideoEncoder::ConfigureMediaTypes() {
    HRESULT hr;

    struct InputConfigAttempt {
        bool setAllSamplesIndependent;
        UINT32 interlaceMode;
    };
    static const InputConfigAttempt kInputConfigs[] = {
        { false, MFVideoInterlace_MixedInterlaceOrProgressive },
        { false, MFVideoInterlace_Progressive },
        { true,  MFVideoInterlace_MixedInterlaceOrProgressive },
    };

    auto ladder = BuildResolutionLadder(m_impl->width, m_impl->height);

    for (auto& [resW, resH] : ladder) {
        // --- Set output type at this resolution ---
        if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }

        hr = MFCreateMediaType(&m_impl->outputType);
        if (FAILED(hr)) continue;

        m_impl->outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_impl->outputType->SetGUID(MF_MT_SUBTYPE,
            m_impl->codecType == 1 ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        MFSetAttributeSize(m_impl->outputType, MF_MT_FRAME_SIZE, resW, resH);
        MFSetAttributeRatio(m_impl->outputType, MF_MT_FRAME_RATE, m_impl->fps, 1);
        {
            uint32_t minBr = ComputeMinBitrate(resW, resH, m_impl->fps);
            if (m_impl->bitrate < minBr) {
                LOG_WARNING("Bitrate %u bps too low for %ux%u@%u fps, "
                           "increasing to %u bps to prevent encoder downscale",
                           m_impl->bitrate, resW, resH, m_impl->fps, minBr);
                m_impl->bitrate = minBr;
            }
        }
        m_impl->outputType->SetUINT32(MF_MT_AVG_BITRATE, m_impl->bitrate);
        m_impl->outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeRatio(m_impl->outputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        // Set codec level to support high resolutions.
        // H.264 Levels 4.x cap at ~1920x1088; Level 5.0+ required above that.
        // HEVC has its own level system but MF_MT_MPEG2_LEVEL uses eAVEncH265VLevel.
        if (m_impl->codecType == 1) {
            // HEVC: Use Level 5.0 for up to 4096x2176@60, Main Profile
            m_impl->outputType->SetUINT32(MF_MT_MPEG2_LEVEL,
                                          static_cast<UINT32>(eAVEncH265VLevel5));
            m_impl->outputType->SetUINT32(MF_MT_MPEG2_PROFILE,
                                          static_cast<UINT32>(1)); // Main Profile
        } else {
            // H.264 level based on frame size (MaxFS in macroblocks) and throughput (MaxMBPS)
            uint32_t mbW = (resW + 15) / 16;
            uint32_t mbH = (resH + 15) / 16;
            uint32_t mbPerFrame = mbW * mbH;
            eAVEncH264VLevel h264Level;
            if (mbPerFrame <= 8160) {
                uint64_t mbPerSec = static_cast<uint64_t>(mbPerFrame) * m_impl->fps;
                if (mbPerSec <= 245760)      h264Level = eAVEncH264VLevel4;
                else if (mbPerSec <= 522240) h264Level = eAVEncH264VLevel4_2;
                else                          h264Level = eAVEncH264VLevel5;
            } else {
                h264Level = eAVEncH264VLevel5;
            }
            m_impl->outputType->SetUINT32(MF_MT_MPEG2_LEVEL, static_cast<UINT32>(h264Level));
            if (h264Level >= eAVEncH264VLevel5) {
                m_impl->outputType->SetUINT32(MF_MT_MPEG2_PROFILE,
                                              static_cast<UINT32>(100)); // High Profile
            }
        }

        hr = m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
        if (FAILED(hr)) {
            LOG_INFO("  Encoder SetOutputType rejected %ux%u: 0x%08X", resW, resH, hr);
            continue;
        }

        // Read back actual frame size the encoder committed to
        {
            UINT32 actualW = 0, actualH = 0;
            MFGetAttributeSize(m_impl->outputType, MF_MT_FRAME_SIZE, &actualW, &actualH);
            if (actualW != resW || actualH != resH) {
                LOG_WARNING("  Encoder changed output from %ux%u to %ux%u",
                           resW, resH, actualW, actualH);
            }
        }

        // --- Set input type at same resolution ---
        if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }
        bool inputOk = false;

        // Try preferred NV12 input types from the MFT first
        for (DWORD i = 0; i < 16; ++i) {
            IMFMediaType* availableType = nullptr;
            hr = m_impl->mft->GetInputAvailableType(0, i, &availableType);
            if (FAILED(hr)) break;

            GUID subtype = GUID_NULL;
            hr = availableType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (SUCCEEDED(hr) && subtype == MFVideoFormat_NV12) {
                // Log native frame size before we override it
                if (i < 3) {
                    UINT32 natW = 0, natH = 0;
                    MFGetAttributeSize(availableType, MF_MT_FRAME_SIZE, &natW, &natH);
                    LOG_INFO("  Input type #%u native frame size: %ux%u", i, natW, natH);
                }

                MFSetAttributeSize(availableType, MF_MT_FRAME_SIZE, resW, resH);
                MFSetAttributeRatio(availableType, MF_MT_FRAME_RATE, m_impl->fps, 1);
                MFSetAttributeRatio(availableType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                availableType->SetUINT32(MF_MT_INTERLACE_MODE,
                                         MFVideoInterlace_MixedInterlaceOrProgressive);

                hr = m_impl->mft->SetInputType(0, availableType, 0);
                if (SUCCEEDED(hr)) {
                    m_impl->inputType = availableType;
                    inputOk = true;
                    break;
                }
            }
            availableType->Release();
        }

        // Fall back to custom NV12 input types
        if (!inputOk) {
            for (auto& cfg : kInputConfigs) {
                if (m_impl->inputType) { m_impl->inputType->Release(); m_impl->inputType = nullptr; }

                hr = MFCreateMediaType(&m_impl->inputType);
                if (FAILED(hr)) continue;

                m_impl->inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                m_impl->inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(m_impl->inputType, MF_MT_FRAME_SIZE, resW, resH);
                MFSetAttributeRatio(m_impl->inputType, MF_MT_FRAME_RATE, m_impl->fps, 1);
                MFSetAttributeRatio(m_impl->inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                m_impl->inputType->SetUINT32(MF_MT_INTERLACE_MODE, cfg.interlaceMode);
                if (cfg.setAllSamplesIndependent) {
                    m_impl->inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                }

                hr = m_impl->mft->SetInputType(0, m_impl->inputType, 0);
                if (SUCCEEDED(hr)) {
                    inputOk = true;
                    break;
                }
            }
        }

        if (!inputOk) {
            LOG_INFO("  Encoder SetInputType rejected %ux%u", resW, resH);
            // Don't recreate the MFT — just release the output type and try
            // the next resolution rung. The caller iterates encoder candidates,
            // so if all rungs fail the next candidate will be tried.
            if (m_impl->outputType) { m_impl->outputType->Release(); m_impl->outputType = nullptr; }
            continue;
        }

        // --- Both output and input accepted at this resolution ---
        {
            // Read back actual frame sizes the encoder committed to
            UINT32 outW = 0, outH = 0, inW = 0, inH = 0;
            if (m_impl->outputType)
                MFGetAttributeSize(m_impl->outputType, MF_MT_FRAME_SIZE, &outW, &outH);
            if (m_impl->inputType)
                MFGetAttributeSize(m_impl->inputType, MF_MT_FRAME_SIZE, &inW, &inH);
            LOG_INFO("Encoder type readback: input=%ux%u output=%ux%u (requested %ux%u)",
                     inW, inH, outW, outH, resW, resH);
        }

        if (resW != m_impl->width || resH != m_impl->height) {
            LOG_WARNING("Encoder using %ux%u (requested %ux%u)",
                       resW, resH, m_impl->width, m_impl->height);
            m_impl->width = resW;
            m_impl->height = resH;
        } else {
            LOG_INFO("%s encoder configured: %ux%u @ %u fps %u bps",
                     m_impl->codecType == 1 ? "HEVC" : "H.264",
                     resW, resH, m_impl->fps, m_impl->bitrate);
        }

        // Enable low latency mode
        IMFAttributes* attrs = nullptr;
        hr = m_impl->mft->GetAttributes(&attrs);
        if (SUCCEEDED(hr)) {
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attrs->Release();
        }

        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        // Read back codec data (SPS/PPS) from the output type
        if (m_impl->outputType) {
            UINT32 blobSize = 0;
            if (SUCCEEDED(m_impl->outputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                                       nullptr, 0, &blobSize)) &&
                blobSize > 0) {
                m_impl->codecData.resize(blobSize);
                if (SUCCEEDED(m_impl->outputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                                           m_impl->codecData.data(),
                                                           blobSize, &blobSize))) {
                    LOG_INFO("Encoder codec data (SPS/PPS): %u bytes", blobSize);
                } else {
                    m_impl->codecData.clear();
                }
            }
        }

        return true;
    }

    LOG_ERROR("No resolution in ladder accepted by encoder (tried %zu rungs from %ux%u)",
              ladder.size(), m_impl->width, m_impl->height);
    return false;
}

// Simple BGRA to NV12 conversion
static void BgraToNv12(const uint8_t* bgra, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& nv12) {
    uint32_t ySize = width * height;
    uint32_t uvSize = width * height / 4;
    nv12.resize(ySize + uvSize * 2);

    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + ySize;

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* src = bgra + (row * width + col) * 4;
            uint8_t b = src[0], g = src[1], r = src[2];

            // BT.601 YUV conversion
            uint8_t y = static_cast<uint8_t>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[row * width + col] = y;

            // Downsampled UV (4:2:0)
            if (row % 2 == 0 && col % 2 == 0) {
                uint8_t u = static_cast<uint8_t>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                uint8_t v = static_cast<uint8_t>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                size_t uvIndex = (row / 2) * (width / 2) + (col / 2);
                uvPlane[uvIndex * 2] = u;
                uvPlane[uvIndex * 2 + 1] = v;
            }
        }
    }
}

bool MfVideoEncoder::EncodeFrame(const uint8_t* rawFrame, uint32_t width, uint32_t height,
                                   std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame) {
    if (!m_impl->initialized) return false;

    outIsKeyFrame = false;

    if (!ProcessInput(rawFrame, width, height)) return false;
    if (!ProcessOutput(outBitstream, outIsKeyFrame)) return false;

    m_impl->frameIndex++;

    // Clear keyframe flag once one is produced
    if (outIsKeyFrame) {
        m_impl->needKeyFrame = false;
    }

    // Force a keyframe periodically or on request
    if (m_impl->needKeyFrame || (m_impl->frameIndex % 120 == 0)) {
        RequestKeyFrame();
    }

    return true;
}

// Bilinear downscale for BGRA frames
static void DownscaleBgra(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                          uint8_t* dst, uint32_t dstW, uint32_t dstH) {
    float scaleX = static_cast<float>(srcW) / dstW;
    float scaleY = static_cast<float>(srcH) / dstH;

    for (uint32_t y = 0; y < dstH; ++y) {
        float srcYf = (y + 0.5f) * scaleY - 0.5f;
        if (srcYf < 0) srcYf = 0;
        uint32_t srcY0 = static_cast<uint32_t>(srcYf);
        uint32_t srcY1 = (srcY0 + 1 < srcH) ? srcY0 + 1 : srcY0;
        float fy = srcYf - srcY0;

        for (uint32_t x = 0; x < dstW; ++x) {
            float srcXf = (x + 0.5f) * scaleX - 0.5f;
            if (srcXf < 0) srcXf = 0;
            uint32_t srcX0 = static_cast<uint32_t>(srcXf);
            uint32_t srcX1 = (srcX0 + 1 < srcW) ? srcX0 + 1 : srcX0;
            float fx = srcXf - srcX0;

            const uint8_t* s00 = src + (srcY0 * srcW + srcX0) * 4;
            const uint8_t* s10 = src + (srcY0 * srcW + srcX1) * 4;
            const uint8_t* s01 = src + (srcY1 * srcW + srcX0) * 4;
            const uint8_t* s11 = src + (srcY1 * srcW + srcX1) * 4;

            uint8_t* d = dst + (y * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                float v0 = s00[c] + (s10[c] - s00[c]) * fx;
                float v1 = s01[c] + (s11[c] - s01[c]) * fx;
                float v  = v0 + (v1 - v0) * fy;
                d[c] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
}

// Upload NV12 data to a D3D11 texture and create a GPU-backed IMFSample.
static bool CreateGpuSample(ID3D11Device* device, ID3D11DeviceContext* context,
                            const uint8_t* nv12Data, uint32_t width, uint32_t height,
                            IMFSample** outSample) {
    *outSample = nullptr;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DYNAMIC;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &texture);
    if (FAILED(hr)) return false;

    // Upload Y plane (subresource 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { texture->Release(); return false; }
        const uint8_t* src = nv12Data;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        for (uint32_t row = 0; row < height; ++row)
            memcpy(dst + row * mapped.RowPitch, src + row * width, width);
        context->Unmap(texture, 0);
    }

    // Upload UV plane (subresource 1)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(texture, 1, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { texture->Release(); return false; }
        const uint8_t* src = nv12Data + static_cast<size_t>(width) * height;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        uint32_t uvH = height / 2;
        for (uint32_t row = 0; row < uvH; ++row)
            memcpy(dst + row * mapped.RowPitch, src + row * width, width);
        context->Unmap(texture, 1);
    }

    // Wrap texture as IMFMediaBuffer via DXGI surface buffer
    IMFMediaBuffer* mediaBuffer = nullptr;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &mediaBuffer);
    texture->Release();
    if (FAILED(hr)) return false;

    hr = MFCreateSample(outSample);
    if (FAILED(hr)) { mediaBuffer->Release(); return false; }

    (*outSample)->AddBuffer(mediaBuffer);
    mediaBuffer->Release();
    return true;
}

bool MfVideoEncoder::ProcessInput(const uint8_t* rawFrame, uint32_t width, uint32_t height) {
    // Downscale if encoder resolution differs from input
    std::vector<uint8_t> scaled;
    const uint8_t* frameData = rawFrame;
    if (width != m_impl->width || height != m_impl->height) {
        scaled.resize(m_impl->width * m_impl->height * 4);
        DownscaleBgra(rawFrame, width, height,
                      scaled.data(), m_impl->width, m_impl->height);
        frameData = scaled.data();
        width = m_impl->width;
        height = m_impl->height;
    }

    // Convert BGRA to NV12
    std::vector<uint8_t> nv12;
    BgraToNv12(frameData, width, height, nv12);

    IMFSample* sample = nullptr;
    HRESULT hr;

    if (m_impl->d3dDevice && m_impl->deviceManager) {
        hr = CreateGpuSample(m_impl->d3dDevice, m_impl->d3dContext,
                            nv12.data(), width, height, &sample);
        if (FAILED(hr)) {
            LOG_WARNING("CreateGpuSample failed: 0x%08X, falling back to CPU", hr);
            sample = nullptr;
        }
    }

    // CPU fallback
    if (!sample) {
        IMFMediaBuffer* mediaBuffer = nullptr;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &mediaBuffer);
        if (FAILED(hr)) return false;

        BYTE* bufferData = nullptr;
        hr = mediaBuffer->Lock(&bufferData, nullptr, nullptr);
        if (FAILED(hr)) {
            mediaBuffer->Release();
            return false;
        }
        memcpy(bufferData, nv12.data(), nv12.size());
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));

        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            mediaBuffer->Release();
            return false;
        }
        sample->AddBuffer(mediaBuffer);
        mediaBuffer->Release();
    }

    // Set timestamp
    LONGLONG duration = 10000000LL / m_impl->fps; // 100ns units
    LONGLONG timestamp = static_cast<LONGLONG>(m_impl->frameIndex) * duration;
    sample->SetSampleTime(timestamp);
    sample->SetSampleDuration(duration);

    hr = m_impl->mft->ProcessInput(0, sample, 0);
    sample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        return true; // Encoder not ready for input yet, try again next frame
    }

    return SUCCEEDED(hr);
}

bool MfVideoEncoder::ProcessOutput(std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame) {
    outBitstream.clear();

    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    MFT_OUTPUT_STREAM_INFO streamInfo = {};

    HRESULT hr = m_impl->mft->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return false;

    // Create output sample if needed
    IMFSample* sample = nullptr;
    if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        IMFMediaBuffer* buf = nullptr;
        hr = MFCreateMemoryBuffer(streamInfo.cbSize ? streamInfo.cbSize : 1048576, &buf);
        if (SUCCEEDED(hr)) {
            hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) {
                sample->AddBuffer(buf);
            }
        }
        if (buf) buf->Release();
        if (sample) {
            outputBuffer.pSample = sample;
            outputBuffer.dwStreamID = 0;
        }
    }

    DWORD status = 0;
    hr = m_impl->mft->ProcessOutput(0, 1, &outputBuffer, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (sample) sample->Release();
        return false;
    }

    if (SUCCEEDED(hr) && outputBuffer.pSample) {
        // Check for keyframe
        if (outputBuffer.pSample->GetUINT32(MFSampleExtension_CleanPoint, (UINT32*)&outIsKeyFrame)
            != S_OK) {
            outIsKeyFrame = false;
        }

        // Extract H.264 bitstream
        DWORD totalLength = 0;
        hr = outputBuffer.pSample->GetTotalLength(&totalLength);
        if (SUCCEEDED(hr) && totalLength > 0) {
            IMFMediaBuffer* buf = nullptr;
            hr = outputBuffer.pSample->ConvertToContiguousBuffer(&buf);
            if (SUCCEEDED(hr)) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                hr = buf->Lock(&data, &maxLen, &curLen);
                if (SUCCEEDED(hr)) {
                    // Check if bitstream contains SPS (NAL type 7 = 0x67 after start code)
                    bool hasSPS = false;
                    if (curLen >= 5) {
                        // Look for Annex B start code + SPS NAL header (0x67)
                        for (DWORD i = 0; i + 4 < curLen; ++i) {
                            if (data[i] == 0x00 && data[i+1] == 0x00) {
                                if (data[i+2] == 0x00 && data[i+3] == 0x01) {
                                    // 4-byte start code found
                                    BYTE nalType = data[i+4] & 0x1F;
                                    if (nalType == 7) { hasSPS = true; break; }
                                    i += 4;
                                } else if (data[i+2] == 0x01) {
                                    // 3-byte start code found
                                    BYTE nalType = data[i+3] & 0x1F;
                                    if (nalType == 7) { hasSPS = true; break; }
                                    i += 3;
                                }
                            }
                        }
                    }

                    // Try to get codec data from output type if we don't have it yet
                    if (m_impl->codecData.empty()) {
                        IMFMediaType* outType = nullptr;
                        if (SUCCEEDED(m_impl->mft->GetOutputCurrentType(0, &outType))) {
                            UINT32 blobSize = 0;
                            if (SUCCEEDED(outType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                                           nullptr, 0, &blobSize)) &&
                                blobSize > 0) {
                                m_impl->codecData.resize(blobSize);
                                if (SUCCEEDED(outType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                                               m_impl->codecData.data(),
                                                               blobSize, &blobSize))) {
                                    LOG_INFO("Encoder codec data (SPS/PPS): %u bytes", blobSize);
                                } else {
                                    m_impl->codecData.clear();
                                }
                            }
                            outType->Release();
                        }
                    }

                    // Prepend codec data if keyframe and bitstream lacks SPS
                    bool missingSPS = outIsKeyFrame && !hasSPS && !m_impl->codecData.empty();
                    size_t prefix = missingSPS ? m_impl->codecData.size() : 0;
                    outBitstream.resize(prefix + curLen);

                    if (missingSPS) {
                        memcpy(outBitstream.data(), m_impl->codecData.data(), prefix);
                        LOG_INFO("Prepending %zu bytes of SPS/PPS codec data (bitstream missing SPS)",
                                 prefix);
                    }

                    memcpy(outBitstream.data() + prefix, data, curLen);
                    buf->Unlock();
                }
                buf->Release();
            }
        }
    }

    if (sample) sample->Release();
    if (outputBuffer.pEvents) outputBuffer.pEvents->Release();

    return !outBitstream.empty();
}

uint32_t MfVideoEncoder::GetWidth() const  { return m_impl->width; }
uint32_t MfVideoEncoder::GetHeight() const { return m_impl->height; }
uint32_t MfVideoEncoder::GetCodecType() const { return m_impl->codecType; }

void MfVideoEncoder::SetBitrate(uint32_t bitrate) {
    if (bitrate == 0) return; // auto — keep current
    uint32_t minBr = ComputeMinBitrate(m_impl->width, m_impl->height, m_impl->fps);
    if (bitrate < minBr) {
        LOG_WARNING("Bitrate %u too low for %ux%u, clamping to %u",
                    bitrate, m_impl->width, m_impl->height, minBr);
        bitrate = minBr;
    }
    m_impl->bitrate = bitrate;
    if (m_impl->outputType) {
        m_impl->outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
    }
}

void MfVideoEncoder::RequestKeyFrame() {
    m_impl->needKeyFrame = true;
    // Flush encoder to force next frame to be a keyframe (IDR)
    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}
