#include "MfVideoDecoder.h"
#include "Common/Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
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
    GUID outputSubtype = GUID_NULL;
    int64_t sampleTime = 0;
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

    // Set input type (H.264)
    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) {
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        hr = m_impl->mft->SetInputType(0, inputType, 0);
        inputType->Release();
        if (FAILED(hr)) {
            LOG_WARNING("Decoder SetInputType failed: 0x%08X (will try auto-detect)", hr);
        }
    }

    // Enumerate available output types — the decoder may not support NV12 directly
    GUID selectedSubtype = GUID_NULL;
    static const GUID* preferredSubtypes[] = {
        &MFVideoFormat_NV12, &MFVideoFormat_YV12, &MFVideoFormat_IYUV,
        &MFVideoFormat_YUY2, &MFVideoFormat_AYUV
    };
    for (const auto* preferred : preferredSubtypes) {
        for (DWORD i = 0; ; ++i) {
            IMFMediaType* availType = nullptr;
            hr = m_impl->mft->GetOutputAvailableType(0, i, &availType);
            if (FAILED(hr)) break;
            GUID subtype = {};
            if (SUCCEEDED(availType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == *preferred) {
                hr = m_impl->mft->SetOutputType(0, availType, 0);
                availType->Release();
                if (SUCCEEDED(hr)) {
                    selectedSubtype = subtype;
                    LOG_INFO("Decoder output type set to {%08X-...}", subtype.Data1);
                    break;
                }
            } else {
                availType->Release();
            }
        }
        if (selectedSubtype != GUID_NULL) break;
    }
    if (selectedSubtype == GUID_NULL) {
        LOG_WARNING("Decoder: could not set any preferred output type, using decoder default");
    }
    m_impl->outputSubtype = selectedSubtype;

    LOG_INFO("H.264 decoder MFT created");
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->initialized = true;
    return true;
}

void MfVideoDecoder::Shutdown() {
    if (m_impl->mft) m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
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

// YV12: Y plane, then V plane, then U plane (planar 4:2:0, like NV12 but separate V/U)
static void Yv12ToBgra(const uint8_t* yv12, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& bgra) {
    bgra.resize(width * height * 4);

    const uint8_t* yPlane = yv12;
    const uint8_t* vPlane = yv12 + width * height;
    const uint8_t* uPlane = yv12 + width * height + (width / 2) * (height / 2);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            int y = yPlane[row * width + col] - 16;
            int uvIndex = (row / 2) * (width / 2) + (col / 2);
            int v = vPlane[uvIndex] - 128;
            int u = uPlane[uvIndex] - 128;

            int r = (298 * y + 409 * v + 128) >> 8;
            int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * y + 516 * u + 128) >> 8;

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

// YUY2: packed 4:2:2, 2 bytes per pixel: [Y0 U Y1 V]
static void Yuy2ToBgra(const uint8_t* yuy2, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& bgra) {
    bgra.resize(width * height * 4);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; col += 2) {
            const uint8_t* src = yuy2 + row * width * 2 + col * 2;
            int y0 = src[0] - 16;
            int u  = src[1] - 128;
            int y1 = src[2] - 16;
            int v  = src[3] - 128;

            for (int i = 0; i < 2; ++i) {
                int y = (i == 0) ? y0 : y1;
                uint32_t px = col + i;
                if (px >= width) break;

                int r = (298 * y + 409 * v + 128) >> 8;
                int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
                int b = (298 * y + 516 * u + 128) >> 8;

                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                g = g < 0 ? 0 : (g > 255 ? 255 : g);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);

                uint8_t* dst = bgra.data() + (row * width + px) * 4;
                dst[0] = static_cast<uint8_t>(b);
                dst[1] = static_cast<uint8_t>(g);
                dst[2] = static_cast<uint8_t>(r);
                dst[3] = 255;
            }
        }
    }
}

// Helper: handle stream format change by reading new output type
static bool HandleStreamChange(IMFTransform* mft, uint32_t& width, uint32_t& height,
                                GUID& subtype, bool& formatEstablished) {
    IMFMediaType* newType = nullptr;
    HRESULT hr = mft->GetOutputCurrentType(0, &newType);
    if (FAILED(hr)) return false;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(newType, MF_MT_FRAME_SIZE, &w, &h);
    width = w;
    height = h;
    GUID actualSubtype = {};
    if (SUCCEEDED(newType->GetGUID(MF_MT_SUBTYPE, &actualSubtype))) {
        subtype = actualSubtype;
    }
    formatEstablished = true;
    LOG_INFO("Decoder stream change: %ux%u subtype=%08X", w, h, subtype.Data1);
    newType->Release();
    return true;
}

// Helper: drain output frames from decoder, handling format changes
// Returns true if at least one frame was decoded
static bool DrainDecoderOutput(IMFTransform* mft, uint32_t& width, uint32_t& height,
                                GUID& subtype, bool& formatEstablished,
                                std::vector<uint8_t>& outRGBA,
                                uint32_t& outWidth, uint32_t& outHeight,
                                int& failCount, size_t inputLen) {
    bool gotFrame = false;
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = mft->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return false;

    DWORD baseBufSize = streamInfo.cbSize ? streamInfo.cbSize : 1920 * 1080 * 3 / 2;

    for (int iter = 0; iter < 5; ++iter) {
        MFT_OUTPUT_DATA_BUFFER outputBuffer = {};

        IMFMediaBuffer* outBuf = nullptr;
        hr = MFCreateMemoryBuffer(baseBufSize, &outBuf);
        if (FAILED(hr)) break;

        IMFSample* outSample = nullptr;
        hr = MFCreateSample(&outSample);
        if (FAILED(hr)) {
            outBuf->Release();
            break;
        }
        outSample->AddBuffer(outBuf);
        outBuf->Release();
        outputBuffer.pSample = outSample;
        outputBuffer.dwStreamID = 0;

        DWORD status = 0;
        hr = mft->ProcessOutput(0, 1, &outputBuffer, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            outSample->Release();
            break;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            outSample->Release();
            HandleStreamChange(mft, width, height, subtype, formatEstablished);
            continue;
        }

        if (FAILED(hr)) {
            outSample->Release();
            if (failCount < 5) {
                LOG_WARNING("Decoder ProcessOutput failed: 0x%08X status=%u (input=%zu bytes)",
                            hr, status, inputLen);
                failCount++;
            }
            break;
        }

        // Extract decoded data
        IMFMediaBuffer* resultBuf = nullptr;
        hr = outputBuffer.pSample->ConvertToContiguousBuffer(&resultBuf);
        if (SUCCEEDED(hr)) {
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            hr = resultBuf->Lock(&data, &maxLen, &curLen);
            if (SUCCEEDED(hr)) {
                if (!formatEstablished) {
                    HandleStreamChange(mft, width, height, subtype, formatEstablished);
                }

                if (subtype == MFVideoFormat_YV12 || subtype == MFVideoFormat_IYUV) {
                    Yv12ToBgra(data, width, height, outRGBA);
                } else if (subtype == MFVideoFormat_YUY2) {
                    Yuy2ToBgra(data, width, height, outRGBA);
                } else {
                    Nv12ToBgra(data, width, height, outRGBA);
                }
                outWidth = width;
                outHeight = height;
                gotFrame = true;

                resultBuf->Unlock();
            }
            resultBuf->Release();
        }

        outSample->Release();

        // After a stream change, we might get the actual frame immediately after
        // Skip the status check and continue to get the real output
    }

    return gotFrame && !outRGBA.empty();
}

bool MfVideoDecoder::DecodeFrame(const uint8_t* bitstream, size_t len,
                                   std::vector<uint8_t>& outRGBA,
                                   uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_impl->initialized || !m_impl->mft || len == 0) return false;
    static int failCount = 0;
    static bool dumpedFirst = false;
    if (!dumpedFirst && len >= 16) {
        LOG_INFO("Decoder first input: size=%zu first16: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                 len,
                 bitstream[0], bitstream[1], bitstream[2], bitstream[3],
                 bitstream[4], bitstream[5], bitstream[6], bitstream[7],
                 bitstream[8], bitstream[9], bitstream[10], bitstream[11],
                 bitstream[12], bitstream[13], bitstream[14], bitstream[15]);
        dumpedFirst = true;
    }

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

    // Set sample time (some decoders require this)
    sample->SetSampleTime(m_impl->sampleTime);
    sample->SetSampleDuration(333333); // ~33ms at 30fps
    m_impl->sampleTime += 333333;

    hr = m_impl->mft->ProcessInput(0, sample, 0);
    sample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        // Decoder has pending output (likely format change) — drain it first, then retry
        DrainDecoderOutput(m_impl->mft, m_impl->width, m_impl->height,
                          m_impl->outputSubtype, m_impl->formatEstablished,
                          outRGBA, outWidth, outHeight, failCount, len);

        // Re-create sample and retry ProcessInput
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &mediaBuffer);
        if (FAILED(hr)) return false;
        hr = mediaBuffer->Lock(&bufferData, nullptr, nullptr);
        if (FAILED(hr)) { mediaBuffer->Release(); return false; }
        memcpy(bufferData, bitstream, len);
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(static_cast<DWORD>(len));
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) { mediaBuffer->Release(); return false; }
        sample->AddBuffer(mediaBuffer);
        mediaBuffer->Release();
        sample->SetSampleTime(m_impl->sampleTime);
        sample->SetSampleDuration(333333);
        hr = m_impl->mft->ProcessInput(0, sample, 0);
        sample->Release();
    }

    if (FAILED(hr)) {
        if (failCount < 5) {
            LOG_WARNING("Decoder ProcessInput failed: 0x%08X (input=%zu bytes)", hr, len);
            failCount++;
        }
        return false;
    }

    // Drain output
    return DrainDecoderOutput(m_impl->mft, m_impl->width, m_impl->height,
                              m_impl->outputSubtype, m_impl->formatEstablished,
                              outRGBA, outWidth, outHeight, failCount, len);
}
