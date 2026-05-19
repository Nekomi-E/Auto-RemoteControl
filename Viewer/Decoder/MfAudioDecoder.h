#pragma once
#include <vector>
#include <cstdint>
#include <memory>

class MfAudioDecoder {
public:
    MfAudioDecoder();
    ~MfAudioDecoder();

    bool Initialize();
    void Shutdown();

    bool DecodeFrame(const std::vector<uint8_t>& aacData,
                     std::vector<uint8_t>& outPCM);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
