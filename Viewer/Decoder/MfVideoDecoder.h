#pragma once
#include <vector>
#include <cstdint>
#include <memory>

class MfVideoDecoder {
public:
    MfVideoDecoder();
    ~MfVideoDecoder();

    bool Initialize(uint32_t codecType, uint32_t width, uint32_t height);
    void Shutdown();

    bool DecodeFrame(const uint8_t* bitstream, size_t len,
                     std::vector<uint8_t>& outRGBA,
                     uint32_t& outWidth, uint32_t& outHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
