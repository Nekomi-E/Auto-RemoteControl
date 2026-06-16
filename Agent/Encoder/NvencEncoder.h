#pragma once
#include "nvEncodeAPI.h"
#include <vector>
#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Direct NVIDIA NVENC encoder using D3D11 texture interop.
// Bypasses Media Foundation MFT entirely, matching Sunshine's
// zero-copy GPU encode pipeline: capture BGRA → NVENC → bitstream.
// Requires nvEncodeAPI64.dll (bundled with NVIDIA driver).
class NvencEncoder {
public:
    NvencEncoder();
    ~NvencEncoder();

    // Initialize with D3D11 device for texture sharing.
    // codec: 0=H.264, 1=HEVC (HEVC requires newer GPU).
    bool Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps,
                    uint32_t codec,
                    ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);
    void Shutdown();

    // Encode a D3D11 BGRA texture directly.  The texture is registered
    // with NVENC on first call and mapped per-frame — zero CPU copy.
    bool EncodeFrameGpu(ID3D11Texture2D* bgraTexture, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetCodecType() const { return m_codec; }

    void SetBitrate(uint32_t bitrate);
    void RequestKeyFrame();

private:
    bool LoadNvencDll();
    bool CreateEncoder();
    bool RegisterInputResources(uint32_t width, uint32_t height, ID3D11Texture2D* bgraTexture);
    bool GetSequenceParams(std::vector<uint8_t>& spsPps);
    void DrainPendingOutput(std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    // DLL handle
    HMODULE m_nvencDll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_nvEnc;
    void* m_encoder = nullptr;
    uint32_t m_apiVersion = 0;  // Negotiated API version from LoadNvencDll

    // Configuration
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_bitrate = 0;
    uint32_t m_fps = 60;
    uint32_t m_codec = 0; // 0=H.264, 1=HEVC
    bool m_initialized = false;
    bool m_needKeyFrame = true;
    uint32_t m_frameIndex = 0;

    // D3D11 device (borrowed from CaptureManager, not owned)
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;

    // Registered resources — one per frame for double-buffering
    struct RegisteredTex {
        void* registeredResource = nullptr;
        void* mappedResource = nullptr;
        ID3D11Texture2D* texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    RegisteredTex m_regTex[2];
    int m_regIdx = 0;

    // SPS/PPS codec data (from GetSequenceParams after init)
    std::vector<uint8_t> m_codecData;

    // Pre-allocated bitstream buffer
    std::vector<uint8_t> m_bitstreamBuffer;
};
