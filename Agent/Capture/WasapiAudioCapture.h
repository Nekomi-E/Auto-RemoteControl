#pragma once
#include <vector>
#include <cstdint>
#include <atomic>
#include <memory>

class WasapiAudioCapture {
public:
    WasapiAudioCapture();
    ~WasapiAudioCapture();

    bool Initialize(uint32_t sampleRate, uint16_t channels);
    void Stop();

    bool GetBuffer(std::vector<uint8_t>& outData);

    bool IsActive() const { return m_initialized; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_initialized = false;
};
