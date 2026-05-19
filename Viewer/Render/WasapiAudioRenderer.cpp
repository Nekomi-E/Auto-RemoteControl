#include "WasapiAudioRenderer.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>

#pragma comment(lib, "ole32.lib")

static const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID IID_IAudioClient = __uuidof(IAudioClient);
static const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

struct WasapiAudioRenderer::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;

    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint32_t frameSize = 4;
    uint32_t bufferFrames = 0;

    std::mutex bufferMutex;
    std::vector<uint8_t> ringBuffer;
    size_t writePos = 0;
    size_t readPos = 0;
    static constexpr size_t RingSize = 48000 * 4 * 2; // 2 seconds of stereo 16-bit

    bool initialized = false;
};

WasapiAudioRenderer::WasapiAudioRenderer() : m_impl(std::make_unique<Impl>()) {
    m_impl->ringBuffer.resize(Impl::RingSize, 0);
}

WasapiAudioRenderer::~WasapiAudioRenderer() { Shutdown(); }

bool WasapiAudioRenderer::Initialize(uint32_t sampleRate, uint16_t channels) {
    m_impl->sampleRate = sampleRate;
    m_impl->channels = channels;
    m_impl->frameSize = channels * 2;

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                   IID_IMMDeviceEnumerator, (void**)&m_impl->enumerator);
    if (FAILED(hr)) return false;

    hr = m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_impl->device);
    if (FAILED(hr)) return false;

    hr = m_impl->device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                   (void**)&m_impl->audioClient);
    if (FAILED(hr)) return false;

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

    REFERENCE_TIME bufferDuration = 500000; // 50ms
    hr = m_impl->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                          bufferDuration, 0, &wf.Format, nullptr);
    if (FAILED(hr)) {
        LOG_WARNING("Audio renderer initialize failed: 0x%08X", hr);
        return false;
    }

    hr = m_impl->audioClient->GetBufferSize(&m_impl->bufferFrames);
    if (FAILED(hr)) return false;

    hr = m_impl->audioClient->GetService(IID_IAudioRenderClient,
                                          (void**)&m_impl->renderClient);
    if (FAILED(hr)) return false;

    m_impl->audioClient->Start();
    m_impl->initialized = true;
    LOG_INFO("WASAPI audio renderer started: %uHz %uch", sampleRate, channels);
    return true;
}

void WasapiAudioRenderer::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->audioClient) m_impl->audioClient->Stop();
    if (m_impl->renderClient) m_impl->renderClient->Release();
    if (m_impl->audioClient) m_impl->audioClient->Release();
    if (m_impl->device) m_impl->device->Release();
    if (m_impl->enumerator) m_impl->enumerator->Release();
    m_impl->renderClient = nullptr;
    m_impl->audioClient = nullptr;
    m_impl->device = nullptr;
    m_impl->enumerator = nullptr;
}

bool WasapiAudioRenderer::Play(const std::vector<uint8_t>& pcmData) {
    if (!m_impl->initialized || !m_impl->renderClient || pcmData.empty()) return false;

    // Write PCM data into ring buffer
    {
        std::lock_guard lock(m_impl->bufferMutex);
        size_t space = Impl::RingSize - ((m_impl->writePos - m_impl->readPos + Impl::RingSize) % Impl::RingSize);
        if (space < pcmData.size() + m_impl->frameSize) return false; // Buffer nearly full

        for (size_t i = 0; i < pcmData.size(); ++i) {
            m_impl->ringBuffer[m_impl->writePos] = pcmData[i];
            m_impl->writePos = (m_impl->writePos + 1) % Impl::RingSize;
        }
    }

    // Check if WASAPI needs more data
    UINT32 padding = 0;
    m_impl->audioClient->GetCurrentPadding(&padding);

    UINT32 available = m_impl->bufferFrames - padding;
    if (available < m_impl->bufferFrames / 4) return true; // Not enough room yet

    // Write available data to WASAPI buffer
    std::lock_guard lock(m_impl->bufferMutex);
    size_t availableBytes = (m_impl->writePos - m_impl->readPos + Impl::RingSize) % Impl::RingSize;
    UINT32 framesToWrite = static_cast<UINT32>(
        (std::min)(availableBytes / m_impl->frameSize, static_cast<size_t>(available)));

    if (framesToWrite == 0) return true;

    BYTE* dest = nullptr;
    HRESULT hr = m_impl->renderClient->GetBuffer(framesToWrite, &dest);
    if (SUCCEEDED(hr) && dest) {
        UINT32 bytesToWrite = framesToWrite * m_impl->frameSize;
        for (UINT32 i = 0; i < bytesToWrite; ++i) {
            dest[i] = m_impl->ringBuffer[m_impl->readPos];
            m_impl->readPos = (m_impl->readPos + 1) % Impl::RingSize;
        }
        m_impl->renderClient->ReleaseBuffer(framesToWrite, 0);
    }

    return true;
}
