#pragma once
#include <vector>
#include <cstdint>
#include <memory>

class MfVideoEncoder {
public:
    MfVideoEncoder();
    ~MfVideoEncoder();

    bool Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps);
    void Shutdown();

    bool EncodeFrame(const uint8_t* rawFrame, uint32_t width, uint32_t height,
                     std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;

    void SetBitrate(uint32_t bitrate);
    void RequestKeyFrame();

private:
    bool InitMFT(bool softwareOnly = false);
    bool ConfigureMediaTypes();
    bool ProcessInput(const uint8_t* rawFrame, uint32_t width, uint32_t height);
    bool ProcessOutput(std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
