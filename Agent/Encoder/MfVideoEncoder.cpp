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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

struct MfVideoEncoder::Impl {
    IMFTransform* mft = nullptr;
    IMFMediaType* inputType = nullptr;
    IMFMediaType* outputType = nullptr;

    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t bitrate = 4000000;
    uint32_t fps = 30;
    uint32_t frameIndex = 0;

    bool initialized = false;
    bool needKeyFrame = true;
};

MfVideoEncoder::MfVideoEncoder() : m_impl(std::make_unique<Impl>()) {}

MfVideoEncoder::~MfVideoEncoder() { Shutdown(); }

bool MfVideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps) {
    m_impl->width = width;
    m_impl->height = height;
    m_impl->bitrate = bitrate;
    m_impl->fps = fps;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: 0x%08X", hr);
        return false;
    }

    if (!InitMFT()) return false;
    if (!ConfigureMediaTypes()) {
        LOG_WARNING("Hardware encoder rejected %ux%u, falling back to software encoder",
                    width, height);
        Shutdown();
        if (MFStartup(MF_VERSION) == S_OK && InitMFT(true) && ConfigureMediaTypes()) {
            LOG_INFO("Software encoder fallback succeeded");
        } else {
            LOG_ERROR("Software encoder fallback also failed");
            return false;
        }
    }

    m_impl->initialized = true;
    return true;
}

void MfVideoEncoder::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->mft) m_impl->mft->Release();
    if (m_impl->inputType) m_impl->inputType->Release();
    if (m_impl->outputType) m_impl->outputType->Release();
    m_impl->mft = nullptr;
    m_impl->inputType = nullptr;
    m_impl->outputType = nullptr;
}

bool MfVideoEncoder::InitMFT(bool softwareOnly) {
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr;

    if (!softwareOnly) {
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                        &inputInfo, &outputInfo,
                        &activates, &count);
        if (FAILED(hr) || count == 0) {
            LOG_WARNING("No hardware H.264 encoder found, trying software...");
            softwareOnly = true;
        }
    }

    if (softwareOnly) {
        // Enumerate all sync MFTs then filter out hardware ones
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT,
                       &inputInfo, &outputInfo,
                       &activates, &count);
        if (SUCCEEDED(hr) && count > 0) {
            // Filter: keep only software MFTs (MF_SA_D3D11_AWARE absent or false)
            UINT32 swCount = 0;
            for (UINT32 i = 0; i < count; ++i) {
                UINT32 d3dAware = 0;
                HRESULT hrAttr = activates[i]->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
                if (FAILED(hrAttr) || d3dAware == 0) {
                    // Software MFT — keep it
                    if (swCount != i) activates[swCount] = activates[i];
                    ++swCount;
                } else {
                    activates[i]->Release();
                }
            }
            count = swCount;
            if (count == 0) {
                CoTaskMemFree(activates);
                hr = E_FAIL;
            }
        }
    }

    if (FAILED(hr) || count == 0) {
        LOG_ERROR("No H.264 encoder available (MFTEnumEx returned hr=0x%08X, count=%u)", hr, count);
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        LOG_ERROR("Failed to activate encoder: 0x%08X", hr);
        return false;
    }

    LOG_INFO("H.264 encoder MFT created successfully (%s)",
             softwareOnly ? "software" : "hardware");
    return true;
}

bool MfVideoEncoder::ConfigureMediaTypes() {
    HRESULT hr;

    // Step 1: Set output type FIRST (required by MF encoders — the MFT needs to
    // know the target format before it can validate input)
    UINT32 tryRes[][2] = {
        { m_impl->width, m_impl->height },
        { 1920, 1080 },
    };

    bool outputOk = false;
    for (auto& res : tryRes) {
        if (m_impl->outputType) m_impl->outputType->Release();
        m_impl->outputType = nullptr;

        hr = MFCreateMediaType(&m_impl->outputType);
        if (FAILED(hr)) return false;

        m_impl->outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_impl->outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(m_impl->outputType, MF_MT_FRAME_SIZE, res[0], res[1]);
        MFSetAttributeRatio(m_impl->outputType, MF_MT_FRAME_RATE, m_impl->fps, 1);
        m_impl->outputType->SetUINT32(MF_MT_AVG_BITRATE, m_impl->bitrate);
        m_impl->outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeRatio(m_impl->outputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
        if (SUCCEEDED(hr)) {
            outputOk = true;
            // If we had to downscale, update width/height
            if (res[0] != m_impl->width || res[1] != m_impl->height) {
                LOG_WARNING("Encoder using %ux%u (requested %ux%u)",
                           res[0], res[1], m_impl->width, m_impl->height);
                m_impl->width = res[0];
                m_impl->height = res[1];
            }
            break;
        }
    }

    if (!outputOk) {
        LOG_ERROR("SetOutputType failed at %ux%u and fallback resolutions",
                  m_impl->width, m_impl->height);
        return false;
    }

    // Step 2: Set input type — try MFT's preferred input types first
    bool inputOk = false;
    for (DWORD i = 0; i < 16; ++i) {
        IMFMediaType* availableType = nullptr;
        hr = m_impl->mft->GetInputAvailableType(0, i, &availableType);
        if (FAILED(hr)) break;

        GUID subtype = GUID_NULL;
        hr = availableType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (SUCCEEDED(hr) && subtype == MFVideoFormat_NV12) {
            // Use this NV12 type, override resolution and framerate
            MFSetAttributeSize(availableType, MF_MT_FRAME_SIZE,
                              m_impl->width, m_impl->height);
            MFSetAttributeRatio(availableType, MF_MT_FRAME_RATE,
                               m_impl->fps, 1);
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

    // If preferred types didn't work, try custom input types
    if (!inputOk) {
        struct ConfigAttempt {
            bool setAllSamplesIndependent;
            UINT32 interlaceMode;
        };
        ConfigAttempt configs[] = {
            { false, MFVideoInterlace_MixedInterlaceOrProgressive },
            { false, MFVideoInterlace_Progressive },
            { true,  MFVideoInterlace_MixedInterlaceOrProgressive },
        };

        for (auto& cfg : configs) {
            if (m_impl->inputType) m_impl->inputType->Release();
            m_impl->inputType = nullptr;

            hr = MFCreateMediaType(&m_impl->inputType);
            if (FAILED(hr)) return false;

            m_impl->inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            m_impl->inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(m_impl->inputType, MF_MT_FRAME_SIZE,
                              m_impl->width, m_impl->height);
            MFSetAttributeRatio(m_impl->inputType, MF_MT_FRAME_RATE,
                               m_impl->fps, 1);
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
        LOG_ERROR("SetInputType failed at %ux%u for all config attempts",
                  m_impl->width, m_impl->height);
        return false;
    }

    // Enable low latency mode
    IMFAttributes* attrs = nullptr;
    hr = m_impl->mft->GetAttributes(&attrs);
    if (SUCCEEDED(hr)) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->Release();
    }

    return true;
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

    // Force a keyframe periodically
    if (m_impl->needKeyFrame || (m_impl->frameIndex % 120 == 0)) {
        RequestKeyFrame();
    }

    return true;
}

// Simple bilinear downscale for BGRA frames
static void DownscaleBgra(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                          uint8_t* dst, uint32_t dstW, uint32_t dstH) {
    for (uint32_t y = 0; y < dstH; ++y) {
        uint32_t srcY = y * srcH / dstH;
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t srcX = x * srcW / dstW;
            const uint8_t* s = src + (srcY * srcW + srcX) * 4;
            uint8_t* d = dst + (y * dstW + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
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

    // Create media buffer
    IMFMediaBuffer* mediaBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &mediaBuffer);
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

    // Create sample
    IMFSample* sample = nullptr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        mediaBuffer->Release();
        return false;
    }
    sample->AddBuffer(mediaBuffer);
    mediaBuffer->Release();

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
                buf->Release();
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
            outBitstream.resize(totalLength);

            IMFMediaBuffer* buf = nullptr;
            hr = outputBuffer.pSample->ConvertToContiguousBuffer(&buf);
            if (SUCCEEDED(hr)) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                hr = buf->Lock(&data, &maxLen, &curLen);
                if (SUCCEEDED(hr)) {
                    memcpy(outBitstream.data(), data, curLen);
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

void MfVideoEncoder::SetBitrate(uint32_t bitrate) {
    m_impl->bitrate = bitrate;
    if (m_impl->outputType) {
        m_impl->outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
    }
}

void MfVideoEncoder::RequestKeyFrame() {
    m_impl->needKeyFrame = true;
    // Send a keyframe request event to the encoder
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(m_impl->mft->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->Release();
    }
    // Some encoders use ProcessMessage for keyframe requests
    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}
