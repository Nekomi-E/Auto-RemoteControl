#pragma once
#include <vector>
#include <cstdint>
#include <memory>

class MfAudioEncoder {
public:
    MfAudioEncoder();
    ~MfAudioEncoder();

    bool Initialize(uint32_t sampleRate, uint16_t channels, uint32_t bitrate);
    void Shutdown();

    bool EncodeFrame(const std::vector<uint8_t>& pcmData,
                     std::vector<uint8_t>& outEncoded);

private:
    bool InitMFT(bool softwareOnly = false);
    bool ConfigureMediaTypes();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
