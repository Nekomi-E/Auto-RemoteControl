#pragma once
#include <vector>
#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class MfVideoDecoder {
public:
    MfVideoDecoder();
    ~MfVideoDecoder();

    bool Initialize(uint32_t codecType, uint32_t width, uint32_t height);
    bool InitializeWithD3D11(uint32_t codecType, uint32_t width, uint32_t height,
                             ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // CPU path: decode to RGBA (existing)
    bool DecodeFrame(const uint8_t* bitstream, size_t len,
                     std::vector<uint8_t>& outRGBA,
                     uint32_t& outWidth, uint32_t& outHeight);

    // GPU path: decode to NV12 GPU texture.
    // Caller receives an AddRef'd ID3D11Texture2D (call Release when done).
    // Returns false if GPU path is unavailable; caller should fall back to DecodeFrame.
    bool DecodeFrameGpu(const uint8_t* bitstream, size_t len,
                        ID3D11Texture2D*& outNv12Texture,
                        uint32_t& outWidth, uint32_t& outHeight);

    bool HasGpuPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
