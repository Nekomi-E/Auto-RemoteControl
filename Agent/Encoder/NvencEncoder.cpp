#include "NvencEncoder.h"
#include "Common/Utils/Logger.h"
#include <d3d11.h>

// The official nvEncodeAPI.h uses NVENCAPI macro for calling convention.
// Ensure it's defined for non-cuda builds.
#ifndef NVENCAPI
#define NVENCAPI __stdcall
#endif

NvencEncoder::NvencEncoder() {
    memset(&m_nvEnc, 0, sizeof(m_nvEnc));
}

NvencEncoder::~NvencEncoder() { Shutdown(); }

bool NvencEncoder::LoadNvencDll() {
    // NVENC D3D11 interop is not available on this system's driver.
    // Skip the lengthy DLL load + version negotiation to save startup time.
    return false;
    if (m_nvencDll) return true;

    m_nvencDll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_nvencDll) {
        LOG_WARNING("NVENC: nvEncodeAPI64.dll not found");
        return false;
    }

    typedef NVENCSTATUS (NVENCAPI* PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
    auto NvEncodeAPICreateInstance =
        (PFN_NvEncodeAPICreateInstance)GetProcAddress(m_nvencDll, "NvEncodeAPICreateInstance");
    if (!NvEncodeAPICreateInstance) {
        LOG_WARNING("NVENC: NvEncodeAPICreateInstance not exported");
        FreeLibrary(m_nvencDll); m_nvencDll = nullptr;
        return false;
    }

    // Version negotiation.  The function-list version field uses
    // NV_ENCODE_API_FUNCTION_LIST_VER which is NVENCAPI_STRUCT_VERSION(2)
    // = NVENCAPI_VERSION | (2<<16) | (0x7<<28).  Different SDK versions
    // have different NVENCAPI_VERSION values embedded.
    // Try the header's own version first, then fall back to older known versions.
    static const uint32_t kVersions[] = {
        NV_ENCODE_API_FUNCTION_LIST_VER,       // v12.1 (this header)
        (uint32_t)((12 | (1 << 24)) | (2 << 16) | (0x7 << 28)), // v12.1 explicit
        (uint32_t)((12 | (0 << 24)) | (2 << 16) | (0x7 << 28)), // v12.0
        (uint32_t)((11 | (1 << 24)) | (2 << 16) | (0x7 << 28)), // v11.1
        (uint32_t)((11 | (0 << 24)) | (2 << 16) | (0x7 << 28)), // v11.0
        (uint32_t)((10 | (0 << 24)) | (2 << 16) | (0x7 << 28)), // v10.0
        (uint32_t)(( 9 | (1 << 24)) | (2 << 16) | (0x7 << 28)), // v9.1
    };

    bool loaded = false;
    uint32_t negotiatedVer = 0;
    for (auto ver : kVersions) {
        memset(&m_nvEnc, 0, sizeof(m_nvEnc));
        m_nvEnc.version = ver;
        int ret = NvEncodeAPICreateInstance(&m_nvEnc);
        if (ret == NV_ENC_SUCCESS) {
            m_apiVersion = ver;
            loaded = true;
            negotiatedVer = ver;
            break;
        }
        if (ret != NV_ENC_ERR_INVALID_VERSION) {
            LOG_WARNING("NVENC: CreateInstance v0x%X returned %d", ver, ret);
            break;
        }
    }

    if (!loaded) {
        LOG_WARNING("NVENC: no compatible API version found");
        FreeLibrary(m_nvencDll); m_nvencDll = nullptr;
        return false;
    }

    LOG_INFO("NVENC: API loaded, negotiated version v0x%X", m_apiVersion);
    return true;
}

bool NvencEncoder::Initialize(uint32_t width, uint32_t height, uint32_t bitrate,
                                uint32_t fps, uint32_t codec,
                                ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
    m_width = width; m_height = height;
    m_bitrate = bitrate; m_fps = fps; m_codec = codec;
    m_d3dDevice = d3dDevice; m_d3dContext = d3dContext;

    if (!LoadNvencDll()) return false;

    // Minimum bitrate check
    uint32_t minBr = (uint32_t)((uint64_t)width * height * fps * 10ULL / 100);
    if (bitrate < minBr) { m_bitrate = minBr; }

    if (!CreateEncoder()) { LOG_WARNING("NVENC: CreateEncoder failed"); return false; }
    if (!GetSequenceParams(m_codecData)) { LOG_WARNING("NVENC: GetSequenceParams failed"); Shutdown(); return false; }

    m_initialized = true;
    LOG_INFO("NVENC: %ux%u @ %u fps %u bps %s", width, height, fps, m_bitrate,
             codec == 1 ? "HEVC" : "H.264");
    return true;
}

bool NvencEncoder::CreateEncoder() {
    // Open encode session with D3D11 device
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams;
    memset(&openParams, 0, sizeof(openParams));
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.device = m_d3dDevice;
    openParams.apiVersion = m_apiVersion;

    int ret = m_nvEnc.nvEncOpenEncodeSessionEx(&openParams, &m_encoder);
    if (ret != NV_ENC_SUCCESS) {
        LOG_WARNING("NVENC: OpenEncodeSessionEx failed: %d", ret);
        return false;
    }

    // Build encoder config manually (Sunshine approach: skip GetEncodePresetConfig
    // which has struct-version compatibility issues across driver versions).
    GUID presetGuid = NV_ENC_PRESET_P1_GUID;

    NV_ENC_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    cfg.gopLength = 240;
    cfg.frameIntervalP = 1;
    cfg.rcParams.version = NV_ENC_RC_PARAMS_VER;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = m_bitrate;
    cfg.rcParams.maxBitRate = m_bitrate * 3 / 2;
    cfg.rcParams.vbvBufferSize = m_bitrate / m_fps * 2;
    cfg.rcParams.vbvInitialDelay = m_bitrate / m_fps;
    cfg.rcParams.enableAQ = 1;

    if (m_codec == 1) {
        cfg.encodeCodecConfig.hevcConfig.idrPeriod = 240;
    } else {
        cfg.encodeCodecConfig.h264Config.idrPeriod = 240;
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    }

    // Try multiple struct versions — drivers may support older formats.
    // Also try 60fps first (some drivers reject 120fps at >1080p).
    struct InitAttempt { uint32_t ver; const char* desc; uint32_t fps; };
    InitAttempt attempts[] = {
        { NVENCAPI_STRUCT_VERSION(1), "v1@60fps", 60 },
        { NVENCAPI_STRUCT_VERSION(5), "v5@60fps", 60 },
        { NV_ENC_INITIALIZE_PARAMS_VER, "full@60fps", 60 },
        { NVENCAPI_STRUCT_VERSION(1), "v1@120fps", 120 },
        { NVENCAPI_STRUCT_VERSION(5), "v5@120fps", 120 },
        { NV_ENC_INITIALIZE_PARAMS_VER, "full@120fps", 120 },
    };

    bool ok = false;
    for (auto& a : attempts) {
        NV_ENC_INITIALIZE_PARAMS initParams;
        memset(&initParams, 0, sizeof(initParams));
        initParams.version = a.ver;
        initParams.encodeWidth = m_width;
        initParams.encodeHeight = m_height;
        initParams.darWidth = m_width;
        initParams.darHeight = m_height;
        initParams.frameRateNum = a.fps;
        initParams.frameRateDen = 1;

        ret = m_nvEnc.nvEncInitializeEncoder(m_encoder, &initParams);
        if (ret == NV_ENC_SUCCESS) {
            ok = true;
            LOG_INFO("NVENC: InitializeEncoder OK with %s (0x%X)", a.desc, a.ver);
            if (a.fps != (int)m_fps) {
                LOG_INFO("NVENC: using %d fps (requested %d)", a.fps, m_fps);
                m_fps = a.fps;
            }
            break;
        }
    }
    if (!ok) {
        LOG_WARNING("NVENC: InitializeEncoder failed with all struct versions + fps combos");
        return false;
    }
    LOG_INFO("NVENC: encoder initialized (manual config, no preset query)");
    return true;
}

bool NvencEncoder::GetSequenceParams(std::vector<uint8_t>& spsPps) {
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload;
    memset(&payload, 0, sizeof(payload));
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = 0;
    payload.outSPSPPSPayloadSize = &size;

    int ret = m_nvEnc.nvEncGetSequenceParams(m_encoder, &payload);
    spsPps.resize(size);
    if (size == 0 && ret != NV_ENC_SUCCESS) return false;

    memset(&payload, 0, sizeof(payload));
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.spsppsBuffer = spsPps.data();
    payload.inBufferSize = size;
    payload.outSPSPPSPayloadSize = &size;

    ret = m_nvEnc.nvEncGetSequenceParams(m_encoder, &payload);
    if (ret != NV_ENC_SUCCESS) { spsPps.clear(); return false; }
    spsPps.resize(size);
    return true;
}

bool NvencEncoder::RegisterInputResources(uint32_t width, uint32_t height,
                                            ID3D11Texture2D* bgraTexture) {
    for (int i = 0; i < 2; ++i) {
        if (m_regTex[i].registeredResource &&
            (m_regTex[i].width != width || m_regTex[i].height != height)) {
            if (m_regTex[i].mappedResource) {
                m_nvEnc.nvEncUnmapInputResource(m_encoder, m_regTex[i].mappedResource);
                m_regTex[i].mappedResource = nullptr;
            }
            m_nvEnc.nvEncUnregisterResource(m_encoder, m_regTex[i].registeredResource);
            m_regTex[i].registeredResource = nullptr;
        }
    }

    for (int i = 0; i < 2; ++i) {
        if (m_regTex[i].registeredResource) continue;

        NV_ENC_REGISTER_RESOURCE reg;
        memset(&reg, 0, sizeof(reg));
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = width;
        reg.height = height;
        reg.pitch = 0;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
        reg.resourceToRegister = bgraTexture;

        int ret = m_nvEnc.nvEncRegisterResource(m_encoder, &reg);
        if (ret != NV_ENC_SUCCESS) {
            LOG_WARNING("NVENC: RegisterResource[%d] failed: %d", i, ret);
            return false;
        }
        m_regTex[i].registeredResource = reg.registeredResource;
        m_regTex[i].width = width;
        m_regTex[i].height = height;
        m_regTex[i].texture = bgraTexture;
    }
    return true;
}

bool NvencEncoder::EncodeFrameGpu(ID3D11Texture2D* bgraTexture,
                                    uint32_t width, uint32_t height,
                                    std::vector<uint8_t>& outBitstream,
                                    bool& outIsKeyFrame) {
    if (!m_initialized || !m_encoder) return false;
    outIsKeyFrame = false;
    outBitstream.clear();

    bool needRegister = false;
    for (int i = 0; i < 2; ++i) {
        if (!m_regTex[i].registeredResource ||
            m_regTex[i].width != width || m_regTex[i].height != height) {
            needRegister = true; break;
        }
    }
    if (needRegister && !RegisterInputResources(width, height, bgraTexture)) return false;

    int idx = m_regIdx;
    m_regIdx ^= 1;

    if (m_regTex[idx].mappedResource) {
        m_nvEnc.nvEncUnmapInputResource(m_encoder, m_regTex[idx].mappedResource);
        m_regTex[idx].mappedResource = nullptr;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapRes;
    memset(&mapRes, 0, sizeof(mapRes));
    mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = m_regTex[idx].registeredResource;

    int ret = m_nvEnc.nvEncMapInputResource(m_encoder, &mapRes);
    if (ret != NV_ENC_SUCCESS) { return false; }
    m_regTex[idx].mappedResource = mapRes.mappedResource;

    NV_ENC_PIC_PARAMS pic;
    memset(&pic, 0, sizeof(pic));
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.inputPitch = 0;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
    pic.inputBuffer = mapRes.mappedResource;
    pic.outputBitstream = 0;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    if (m_needKeyFrame || (m_frameIndex % 240 == 0))
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;

    ret = m_nvEnc.nvEncEncodePicture(m_encoder, &pic);
    if (ret == NV_ENC_ERR_NEED_MORE_INPUT) { m_frameIndex++; m_needKeyFrame = false; return false; }
    if (ret != NV_ENC_SUCCESS) { return false; }

    NV_ENC_LOCK_BITSTREAM lock;
    memset(&lock, 0, sizeof(lock));
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.doNotWait = 0;

    ret = m_nvEnc.nvEncLockBitstream(m_encoder, &lock);
    if (ret != NV_ENC_SUCCESS) { return false; }

    if (lock.bitstreamSizeInBytes > 0) {
        outBitstream.assign((const uint8_t*)lock.outputBitstream,
                            (const uint8_t*)lock.outputBitstream + lock.bitstreamSizeInBytes);
        if (pic.encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR) {
            outIsKeyFrame = true;
            if (!m_codecData.empty())
                outBitstream.insert(outBitstream.begin(), m_codecData.begin(), m_codecData.end());
        }
    }

    m_nvEnc.nvEncUnlockBitstream(m_encoder, lock.outputBitstream);
    m_frameIndex++;
    if (outIsKeyFrame) m_needKeyFrame = false;
    if (m_frameIndex % 240 == 0) m_needKeyFrame = true;
    return !outBitstream.empty();
}

void NvencEncoder::SetBitrate(uint32_t bitrate) { m_bitrate = bitrate; }
void NvencEncoder::RequestKeyFrame() { m_needKeyFrame = true; }

void NvencEncoder::Shutdown() {
    m_initialized = false;
    if (m_encoder) {
        for (int i = 0; i < 2; ++i) {
            if (m_regTex[i].mappedResource)
                m_nvEnc.nvEncUnmapInputResource(m_encoder, m_regTex[i].mappedResource);
            if (m_regTex[i].registeredResource)
                m_nvEnc.nvEncUnregisterResource(m_encoder, m_regTex[i].registeredResource);
            m_regTex[i] = {};
        }
        NV_ENC_PIC_PARAMS pic; memset(&pic, 0, sizeof(pic));
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
        m_nvEnc.nvEncEncodePicture(m_encoder, &pic);
        m_nvEnc.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
    if (m_nvencDll) { FreeLibrary(m_nvencDll); m_nvencDll = nullptr; }
    memset(&m_nvEnc, 0, sizeof(m_nvEnc));
    m_width = m_height = m_bitrate = 0;
    m_fps = 60; m_frameIndex = 0; m_needKeyFrame = true;
}
