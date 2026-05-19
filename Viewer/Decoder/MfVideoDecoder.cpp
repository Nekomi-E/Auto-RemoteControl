#include "MfVideoDecoder.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

struct MfVideoDecoder::Impl {
    IMFTransform* mft = nullptr;
    IMFMediaType* inputType = nullptr;
    IMFMediaType* outputType = nullptr;
    bool initialized = false;
    uint32_t width = 0;
    uint32_t height = 0;
    bool formatEstablished = false;
};

MfVideoDecoder::MfVideoDecoder() : m_impl(std::make_unique<Impl>()) {}

MfVideoDecoder::~MfVideoDecoder() { Shutdown(); }

bool MfVideoDecoder::Initialize() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: 0x%08X", hr);
        return false;
    }

    // Find H.264 decoder
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                   &inputInfo, &outputInfo, &activates, &count);

    if (FAILED(hr) || count == 0) {
        // Try software
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT,
                       &inputInfo, &outputInfo, &activates, &count);
    }
    if (FAILED(hr) || count == 0) {
        LOG_ERROR("No H.264 decoder found (MFTEnumEx returned hr=0x%08X, count=%u)", hr, count);
        return false;
    } else {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) return false;
    }

    LOG_INFO("H.264 decoder MFT created");
    m_impl->initialized = true;
    return true;
}

void MfVideoDecoder::Shutdown() {
    m_impl->initialized = false;
    if (m_impl->mft) m_impl->mft->Release();
    if (m_impl->inputType) m_impl->inputType->Release();
    if (m_impl->outputType) m_impl->outputType->Release();
    m_impl->mft = nullptr;
}

static void Nv12ToBgra(const uint8_t* nv12, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& bgra) {
    bgra.resize(width * height * 4);

    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + width * height;

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            int y = yPlane[row * width + col] - 16;
            int uvIndex = (row / 2) * (width / 2) + (col / 2);
            int u = uvPlane[uvIndex * 2] - 128;
            int v = uvPlane[uvIndex * 2 + 1] - 128;

            // BT.601 YUV to RGB
            int r = (298 * y + 409 * v + 128) >> 8;
            int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * y + 516 * u + 128) >> 8;

            // Clamp
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);

            uint8_t* dst = bgra.data() + (row * width + col) * 4;
            dst[0] = static_cast<uint8_t>(b);
            dst[1] = static_cast<uint8_t>(g);
            dst[2] = static_cast<uint8_t>(r);
            dst[3] = 255;
        }
    }
}

bool MfVideoDecoder::DecodeFrame(const uint8_t* bitstream, size_t len,
                                   std::vector<uint8_t>& outRGBA,
                                   uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_impl->initialized || !m_impl->mft || len == 0) return false;

    // Create input sample
    IMFMediaBuffer* mediaBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &mediaBuffer);
    if (FAILED(hr)) return false;

    BYTE* bufferData = nullptr;
    hr = mediaBuffer->Lock(&bufferData, nullptr, nullptr);
    if (FAILED(hr)) {
        mediaBuffer->Release();
        return false;
    }
    memcpy(bufferData, bitstream, len);
    mediaBuffer->Unlock();
    mediaBuffer->SetCurrentLength(static_cast<DWORD>(len));

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
    DWORD bufSize = streamInfo.cbSize ? streamInfo.cbSize : 1920 * 1080 * 3 / 2;
    hr = MFCreateMemoryBuffer(bufSize, &outBuf);
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

    // Extract decoded NV12 data
    IMFMediaBuffer* resultBuf = nullptr;
    hr = outputBuffer.pSample->ConvertToContiguousBuffer(&resultBuf);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = resultBuf->Lock(&data, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            // If we haven't established format yet, read from output type
            if (!m_impl->formatEstablished) {
                IMFMediaType* outType = nullptr;
                if (SUCCEEDED(m_impl->mft->GetOutputCurrentType(0, &outType))) {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(outType, MF_MT_FRAME_SIZE, &w, &h);
                    m_impl->width = w;
                    m_impl->height = h;
                    m_impl->formatEstablished = true;
                    outType->Release();
                } else {
                    m_impl->width = 1920;
                    m_impl->height = 1080;
                }
            }

            Nv12ToBgra(data, m_impl->width, m_impl->height, outRGBA);
            outWidth = m_impl->width;
            outHeight = m_impl->height;

            resultBuf->Unlock();
        }
        resultBuf->Release();
    }

    outSample->Release();
    return !outRGBA.empty();
}
