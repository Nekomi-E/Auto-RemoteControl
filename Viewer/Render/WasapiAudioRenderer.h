#pragma once
#include <vector>
#include <cstdint>
#include <memory>

class WasapiAudioRenderer {
public:
    WasapiAudioRenderer();
    ~WasapiAudioRenderer();

    bool Initialize(uint32_t sampleRate, uint16_t channels);
    void Shutdown();
    bool Play(const std::vector<uint8_t>& pcmData);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_initialized = false;
};
