#include "MfAudioEncoder.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

struct MfAudioEncoder::Impl {
    IMFTransform* mft = nullptr;
    IMFMediaType* inputType = nullptr;
    IMFMediaType* outputType = nullptr;

    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint32_t bitrate = 64000;
    uint32_t inputSamplesPerFrame = 1024; // AAC frame size

    bool initialized = false;
};

MfAudioEncoder::MfAudioEncoder() : m_impl(std::make_unique<Impl>()) {}

MfAudioEncoder::~MfAudioEncoder() { Shutdown(); }

bool MfAudioEncoder::Initialize(uint32_t sampleRate, uint16_t channels, uint32_t bitrate) {
    m_impl->sampleRate = sampleRate;
    m_impl->channels = channels;
    m_impl->bitrate = bitrate;

    if (!InitMFT()) return false;
    if (!ConfigureMediaTypes()) {
        LOG_WARNING("Hardware audio encoder rejected config, falling back to software");
        Shutdown();
        if (InitMFT(true) && ConfigureMediaTypes()) {
            LOG_INFO("Software audio encoder fallback succeeded");
        } else {
            LOG_WARNING("Software audio encoder fallback also failed");
            return false;
        }
    }

    m_impl->initialized = true;
    LOG_INFO("AAC audio encoder ready: %uHz %uch %ubps",
             sampleRate, channels, bitrate);
    return true;
}

bool MfAudioEncoder::InitMFT(bool softwareOnly) {
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr;

    if (!softwareOnly) {
        // Try hardware encoder first
        hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                        &inputInfo, &outputInfo,
                        &activates, &count);
    }

    if (softwareOnly || FAILED(hr) || count == 0) {
        // Fall back to any sync encoder (no filtering — first attempt needs any encoder)
        if (!softwareOnly && count > 0) {
            for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT,
                       &inputInfo, &outputInfo,
                       &activates, &count);

        // If explicit software-only, filter out D3D11-aware MFTs
        if (softwareOnly && SUCCEEDED(hr) && count > 0) {
            UINT32 swCount = 0;
            for (UINT32 i = 0; i < count; ++i) {
                UINT32 d3dAware = 0;
                HRESULT hrAttr = activates[i]->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
                if (FAILED(hrAttr) || d3dAware == 0) {
                    if (swCount != i) activates[swCount] = activates[i];
                    ++swCount;
                } else {
                    activates[i]->Release();
                }
            }
            count = swCount;
            if (count == 0) {
                CoTaskMemFree(activates);
                hr = E_FAIL;
            }
        }
    }

    if (FAILED(hr) || count == 0) {
        LOG_WARNING("No AAC encoder found (MFTEnumEx: 0x%08X, count=%u)", hr, count);
        return false;
    }

    LOG_INFO("Found %u AAC encoder candidate(s)%s", count,
             softwareOnly ? " (software-only)" : "");

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        LOG_WARNING("Failed to activate AAC encoder: 0x%08X", hr);
        return false;
    }

    return true;
}

bool MfAudioEncoder::ConfigureMediaTypes() {
    HRESULT hr;

    // Set input type (PCM) — use custom type (original approach worked for SetInputType)
    hr = MFCreateMediaType(&m_impl->inputType);
    if (FAILED(hr)) return false;

    m_impl->inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    m_impl->inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    m_impl->inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_impl->channels);
    m_impl->inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_impl->sampleRate);
    m_impl->inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    m_impl->inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, m_impl->channels * 2);
    m_impl->inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                  m_impl->sampleRate * m_impl->channels * 2);
    m_impl->inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = m_impl->mft->SetInputType(0, m_impl->inputType, 0);
    if (FAILED(hr)) {
        LOG_WARNING("Audio encoder SetInputType failed: 0x%08X", hr);
        return false;
    }

    // Set output type (AAC) with essential attributes
    hr = MFCreateMediaType(&m_impl->outputType);
    if (FAILED(hr)) return false;

    m_impl->outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    m_impl->outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    m_impl->outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_impl->channels);
    m_impl->outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_impl->sampleRate);
    m_impl->outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_impl->bitrate / 8);
    m_impl->outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1); // 1=ADTS

    hr = m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
    if (FAILED(hr)) {
        // Try without AAC payload type
        m_impl->outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // 0=raw AAC
        hr = m_impl->mft->SetOutputType(0, m_impl->outputType, 0);
    }
    if (FAILED(hr)) {
        LOG_WARNING("Audio encoder SetOutputType failed: 0x%08X", hr);
        return false;
    }

    return true;
}

void MfAudioEncoder::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->mft) m_impl->mft->Release();
    if (m_impl->inputType) m_impl->inputType->Release();
    if (m_impl->outputType) m_impl->outputType->Release();
    m_impl->mft = nullptr;
    m_impl->inputType = nullptr;
    m_impl->outputType = nullptr;
}

bool MfAudioEncoder::EncodeFrame(const std::vector<uint8_t>& pcmData,
                                   std::vector<uint8_t>& outEncoded) {
    if (!m_impl->initialized || !m_impl->mft || pcmData.empty()) return false;

    // Create input sample
    IMFMediaBuffer* mediaBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(pcmData.size()), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* bufferData = nullptr;
    hr = mediaBuffer->Lock(&bufferData, nullptr, nullptr);
    if (FAILED(hr)) {
        mediaBuffer->Release();
        return false;
    }
    memcpy(bufferData, pcmData.data(), pcmData.size());
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(static_cast<DWORD>(pcmData.size()));

    IMFSample* sample = nullptr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        mediaBuffer->Release();
        return false;
    }
    sample->AddBuffer(mediaBuffer);
    mediaBuffer->Release();

    hr = m_impl->mft->ProcessInput(0, sample, 0);
    sample->Release();
    if (hr == MF_E_NOTACCEPTING) return false;
    if (FAILED(hr)) return false;

    // Get output
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    hr = m_impl->mft->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* outBuf = nullptr;
    hr = MFCreateMemoryBuffer(streamInfo.cbSize ? streamInfo.cbSize : 65536, &outBuf);
    if (FAILED(hr)) return false;

    IMFSample* outSample = nullptr;
    hr = MFCreateSample(&outSample);
    if (SUCCEEDED(hr)) {
        outSample->AddBuffer(outBuf);
        outBuf->Release();
        outputBuffer.pSample = outSample;
        outputBuffer.dwStreamID = 0;
    } else {
        outBuf->Release();
        return false;
    }

    DWORD status = 0;
    hr = m_impl->mft->ProcessOutput(0, 1, &outputBuffer, &status);
    if (FAILED(hr) || !outputBuffer.pSample) {
        outSample->Release();
        return false;
    }

    IMFMediaBuffer* resultBuf = nullptr;
    hr = outputBuffer.pSample->ConvertToContiguousBuffer(&resultBuf);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = resultBuf->Lock(&data, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            outEncoded.assign(data, data + curLen);
            resultBuf->Unlock();
        }
        resultBuf->Release();
    }

    outSample->Release();
    return !outEncoded.empty();
}
