#include "WasapiAudioCapture.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <thread>
#include <mutex>

#pragma comment(lib, "ole32.lib")

static const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient = __uuidof(IAudioClient);
static const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

struct WasapiAudioCapture::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;

    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint32_t frameSize = 4; // 16-bit stereo = 4 bytes/frame

    std::atomic<bool> running{false};
    std::mutex bufferMutex;
    std::vector<uint8_t> buffer;
    std::atomic<bool> hasData{false};

    HANDLE eventHandle = nullptr;
    std::thread captureThread;
};

WasapiAudioCapture::WasapiAudioCapture() : m_impl(std::make_unique<Impl>()) {}

WasapiAudioCapture::~WasapiAudioCapture() { Stop(); }

bool WasapiAudioCapture::Initialize(uint32_t sampleRate, uint16_t channels) {
    m_impl->sampleRate = sampleRate;
    m_impl->channels = channels;
    m_impl->frameSize = channels * 2; // 16-bit

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   IID_IMMDeviceEnumerator, (void**)&m_impl->enumerator);
    if (FAILED(hr)) {
        LOG_WARNING("Failed to create MMDeviceEnumerator: 0x%08X", hr);
        return false;
    }

    // Get default audio render endpoint for loopback capture
    hr = m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_impl->device);
    if (FAILED(hr)) {
        LOG_WARNING("Failed to get default audio endpoint: 0x%08X", hr);
        return false;
    }

    hr = m_impl->device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                   (void**)&m_impl->audioClient);
    if (FAILED(hr)) {
        LOG_WARNING("Failed to activate audio client: 0x%08X", hr);
        return false;
    }

    WAVEFORMATEXTENSIBLE wf = {};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = channels;
    wf.Format.nSamplesPerSec = sampleRate;
    wf.Format.nBlockAlign = channels * 2;
    wf.Format.nAvgBytesPerSec = sampleRate * channels * 2;
    wf.Format.wBitsPerSample = 16;
    wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 16;
    wf.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                      : SPEAKER_FRONT_CENTER;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    // Try loopback mode with event-driven buffering
    hr = m_impl->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          1000000, // 1 second buffer
                                          0,
                                          &wf.Format, nullptr);
    if (FAILED(hr)) {
        // Retry without event callback
        hr = m_impl->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK,
                                              1000000, 0, &wf.Format, nullptr);
    }

    if (FAILED(hr)) {
        LOG_WARNING("Failed to initialize audio client: 0x%08X", hr);
        return false;
    }

    hr = m_impl->audioClient->GetService(IID_IAudioCaptureClient,
                                          (void**)&m_impl->captureClient);
    if (FAILED(hr)) {
        LOG_WARNING("Failed to get capture client: 0x%08X", hr);
        return false;
    }

    m_impl->audioClient->Start();
    m_impl->running = true;
    m_initialized = true;
    LOG_INFO("WASAPI audio capture started: %uHz, %u channels", sampleRate, channels);
    return true;
}

void WasapiAudioCapture::Stop() {
    m_impl->running = false;
    if (m_impl->captureThread.joinable()) m_impl->captureThread.join();
    if (m_impl->audioClient) m_impl->audioClient->Stop();
    if (m_impl->captureClient) m_impl->captureClient->Release();
    if (m_impl->audioClient) m_impl->audioClient->Release();
    if (m_impl->device) m_impl->device->Release();
    if (m_impl->enumerator) m_impl->enumerator->Release();
    m_initialized = false;
}

bool WasapiAudioCapture::GetBuffer(std::vector<uint8_t>& outData) {
    if (!m_impl->running || !m_impl->captureClient) return false;

    UINT32 packetLength = 0;
    HRESULT hr = m_impl->captureClient->GetNextPacketSize(&packetLength);
    if (FAILED(hr) || packetLength == 0) return false;

    while (packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;

        hr = m_impl->captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (SUCCEEDED(hr) && data && numFrames > 0) {
            uint32_t bytes = numFrames * m_impl->frameSize;

            std::lock_guard lock(m_impl->bufferMutex);
            size_t offset = m_impl->buffer.size();
            m_impl->buffer.resize(offset + bytes);
            memcpy(m_impl->buffer.data() + offset, data, bytes);
            m_impl->hasData = true;
        }
        m_impl->captureClient->ReleaseBuffer(numFrames);
        m_impl->captureClient->GetNextPacketSize(&packetLength);
    }

    // Return accumulated buffer if large enough (~20ms of audio)
    const uint32_t minSize = m_impl->sampleRate * m_impl->frameSize * 20 / 1000;

    std::lock_guard lock(m_impl->bufferMutex);
    if (m_impl->buffer.size() >= minSize) {
        outData = std::move(m_impl->buffer);
        m_impl->buffer.clear();
        m_impl->buffer.reserve(minSize * 2);
        m_impl->hasData = false;
        return true;
    }
    return false;
}
