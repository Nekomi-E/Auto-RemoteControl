#include "MfAudioDecoder.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

struct MfAudioDecoder::Impl {
    IMFTransform* mft = nullptr;
    bool initialized = false;
};

MfAudioDecoder::MfAudioDecoder() : m_impl(std::make_unique<Impl>()) {}

MfAudioDecoder::~MfAudioDecoder() { Shutdown(); }

bool MfAudioDecoder::Initialize() {
    // Find AAC decoder
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_PCM };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT,
                            &inputInfo, &outputInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
        LOG_WARNING("No AAC decoder found (MFTEnumEx returned hr=0x%08X, count=%u)", hr, count);
        if (activates) CoTaskMemFree(activates);
        return false;
    } else {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) return false;
    }

    LOG_INFO("AAC decoder MFT created");
    m_impl->initialized = true;
    return true;
}

void MfAudioDecoder::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->mft) {
        m_impl->mft->Release();
        m_impl->mft = nullptr;
    }
}

bool MfAudioDecoder::DecodeFrame(const std::vector<uint8_t>& aacData,
                                   std::vector<uint8_t>& outPCM) {
    if (!m_impl->initialized || !m_impl->mft || aacData.empty()) return false;

    // Create input sample
    IMFMediaBuffer* mediaBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(aacData.size()), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* bufferData = nullptr;
    hr = mediaBuffer->Lock(&bufferData, nullptr, nullptr);
    if (FAILED(hr)) {
        mediaBuffer->Release();
        return false;
    }
    memcpy(bufferData, aacData.data(), aacData.size());
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(static_cast<DWORD>(aacData.size()));

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
            outPCM.assign(data, data + curLen);
            resultBuf->Unlock();
        }
        resultBuf->Release();
    }

    outSample->Release();
    return !outPCM.empty();
}
