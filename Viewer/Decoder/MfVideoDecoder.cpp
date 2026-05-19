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
    int32_t yStride = 0;
    bool formatEstablished = false;
    GUID outputSubtype = GUID_NULL;
    GUID inputSubtype = GUID_NULL;
    int64_t sampleTime = 0;
};

MfVideoDecoder::MfVideoDecoder() : m_impl(std::make_unique<Impl>()) {}

MfVideoDecoder::~MfVideoDecoder() { Shutdown(); }

bool MfVideoDecoder::Initialize(uint32_t codecType, uint32_t width, uint32_t height) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: 0x%08X", hr);
        return false;
    }

    GUID inputSubtype = (codecType == 1) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    const char* codecName = (codecType == 1) ? "HEVC" : "H.264";
    m_impl->inputSubtype = inputSubtype;

    // Find decoder for the requested codec
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, inputSubtype };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
                   &inputInfo, &outputInfo, &activates, &count);

    if (FAILED(hr) || count == 0) {
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT,
                       &inputInfo, &outputInfo, &activates, &count);
    }
    if (FAILED(hr) || count == 0) {
        LOG_ERROR("No %s decoder found (MFTEnumEx returned hr=0x%08X, count=%u)",
                  codecName, hr, count);
        return false;
    } else {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) return false;
    }

    // Set input type with known frame size so the decoder can negotiate output types.
    // HEVC decoders (e.g. Microsoft HEVC Video Extension) require a complete input
    // media type and will reject ProcessInput with MF_E_TRANSFORM_TYPE_NOT_SET otherwise.
    bool inputTypeSet = false;
    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) {
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
        MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, width, height);
        IMFAttributes* inputAttrs = nullptr;
        if (SUCCEEDED(inputType->QueryInterface(IID_PPV_ARGS(&inputAttrs)))) {
            inputAttrs->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            inputAttrs->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            inputAttrs->Release();
        }
        hr = m_impl->mft->SetInputType(0, inputType, 0);
        if (SUCCEEDED(hr)) {
            inputTypeSet = true;
            LOG_INFO("Decoder input type set: %ux%u %s", width, height, codecName);
        } else {
            LOG_WARNING("Decoder SetInputType(%ux%u) failed: 0x%08X, trying without frame size",
                        width, height, hr);
            // Retry with bare type (no frame size) as fallback
            inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
            hr = m_impl->mft->SetInputType(0, inputType, 0);
            if (SUCCEEDED(hr)) {
                inputTypeSet = true;
            } else {
                LOG_WARNING("Decoder SetInputType (bare) failed: 0x%08X", hr);
            }
        }
        inputType->Release();
    }

    // Enumerate available output types
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

    LOG_INFO("%s decoder MFT created", codecName);
    if (inputTypeSet) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
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

// NV12: Y plane (stride=yStride), then interleaved UV plane (same stride).
// uvOffset: byte offset from start to UV plane.
//   When 0 (default): uvOffset = yStride * height (tightly-packed buffer).
//   Set explicitly when content sits in a larger buffer surface.
static void Nv12ToBgra(const uint8_t* nv12, uint32_t width, uint32_t height,
                        int32_t yStride, std::vector<uint8_t>& bgra,
                        size_t uvOffset = 0) {
    bgra.resize(width * height * 4);

    const uint8_t* yPlane = nv12;
    size_t uvOff = uvOffset ? uvOffset : static_cast<size_t>(yStride) * height;
    const uint8_t* uvPlane = nv12 + uvOff;

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            int y = yPlane[row * yStride + col] - 16;
            int uvRowOff = (row / 2) * yStride + (col / 2) * 2;
            int u = uvPlane[uvRowOff] - 128;
            int v = uvPlane[uvRowOff + 1] - 128;

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

// YV12: Y (stride=yStride), V (stride=yStride/2), U (stride=yStride/2)
static void Yv12ToBgra(const uint8_t* yv12, uint32_t width, uint32_t height,
                        int32_t yStride, std::vector<uint8_t>& bgra) {
    bgra.resize(width * height * 4);

    int32_t cStride = yStride / 2;
    const uint8_t* yPlane = yv12;
    const uint8_t* vPlane = yv12 + yStride * height;
    const uint8_t* uPlane = vPlane + cStride * (height / 2);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            int y = yPlane[row * yStride + col] - 16;
            int cOff = (row / 2) * cStride + (col / 2);
            int v = vPlane[cOff] - 128;
            int u = uPlane[cOff] - 128;

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

// YUY2: packed [Y0 U Y1 V], stride=yuy2Stride bytes per row
static void Yuy2ToBgra(const uint8_t* yuy2, uint32_t width, uint32_t height,
                        int32_t yuy2Stride, std::vector<uint8_t>& bgra) {
    bgra.resize(width * height * 4);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; col += 2) {
            const uint8_t* src = yuy2 + row * yuy2Stride + col * 2;
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
                                int32_t& yStride, GUID& subtype, bool& formatEstablished) {
    IMFMediaType* newType = nullptr;
    HRESULT hr = mft->GetOutputCurrentType(0, &newType);
    if (FAILED(hr)) return false;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(newType, MF_MT_FRAME_SIZE, &w, &h);
    width = w;
    height = h;

    // Read the actual row stride (may be > width due to alignment)
    yStride = static_cast<int32_t>(w);  // default fallback
    UINT32 strideVal = 0;
    if (SUCCEEDED(newType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideVal)) && strideVal > 0) {
        yStride = static_cast<int32_t>(strideVal);
    }

    GUID actualSubtype = {};
    if (SUCCEEDED(newType->GetGUID(MF_MT_SUBTYPE, &actualSubtype))) {
        subtype = actualSubtype;
    }
    formatEstablished = true;
    LOG_INFO("Decoder stream change: %ux%u stride=%d subtype=%08X",
             w, h, yStride, subtype.Data1);
    newType->Release();
    return true;
}

// Compute the actual buffer stride and UV-plane offset from the buffer length.
// contentWidth/contentHeight are from the media type (SPS) — kept unchanged.
// The decoder surface may differ from the encoded picture due to:
//   - Encoder padding to macroblock boundaries (multiples of 16)
//   - Encoder using a different output resolution than SPS reports
//   - GPU surface pool being larger than the coded picture
// NV12: bufLen = bufferStride * bufferHeight * 3/2 (contiguous buffer).
static bool ComputeNv12BufferLayout(DWORD bufLen, uint32_t contentWidth, uint32_t contentHeight,
                                     int32_t& outStride, size_t& outUvOffset) {
    size_t expected = static_cast<size_t>(contentWidth) * contentHeight * 3 / 2;
    if (bufLen == 0 || bufLen == expected) return false;

    uint64_t product = static_cast<uint64_t>(bufLen) * 2 / 3;

    // Search outward from contentWidth in steps of 2.
    // Encoder typically pads to macroblock-aligned sizes, so the
    // real stride is usually within a few hundred pixels of contentWidth.
    for (int32_t delta = 0; delta <= 512; ++delta) {
        int32_t candidates[2] = {
            static_cast<int32_t>(contentWidth) - delta,
            static_cast<int32_t>(contentWidth) + delta
        };
        for (int i = 0; i < 2; ++i) {
            int32_t tryStride = candidates[i];
            if (delta == 0 && i == 1) continue;  // only try contentWidth once
            if (tryStride < 64 || tryStride > 4096) continue;
            if (product % tryStride != 0) continue;

            uint32_t bufferHeight = static_cast<uint32_t>(product / tryStride);
            // Accept if bufferHeight is within plausible range of contentHeight
            if (bufferHeight >= contentHeight && bufferHeight <= contentHeight + 256) {
                outStride = tryStride;
                outUvOffset = static_cast<size_t>(tryStride) * bufferHeight;
                static bool loggedOnce = false;
                if (!loggedOnce) {
                    LOG_INFO("NV12 buffer layout: content=%ux%u stride=%d bufHeight=%u uvOff=%zu (bufLen=%u)",
                             contentWidth, contentHeight, outStride, bufferHeight, outUvOffset, bufLen);
                    loggedOnce = true;
                }
                return true;
            }
        }
    }

    // Fallback: common strides with looser height constraint (for larger surfaces)
    static const uint32_t commonStrides[] = {
        3840, 3440, 2880, 2560, 2048, 1920, 1856, 1680, 1600, 1440,
        1366, 1360, 1280, 1152, 1024, 960, 800, 768, 720, 640
    };
    for (uint32_t tryStride : commonStrides) {
        if (product % tryStride == 0) {
            uint32_t bufferHeight = static_cast<uint32_t>(product / tryStride);
            if (bufferHeight >= contentHeight) {
                outStride = static_cast<int32_t>(tryStride);
                outUvOffset = static_cast<size_t>(tryStride) * bufferHeight;
                static bool loggedFallback = false;
                if (!loggedFallback) {
                    LOG_INFO("NV12 buffer layout (fallback): content=%ux%u stride=%d bufHeight=%u uvOff=%zu (bufLen=%u)",
                             contentWidth, contentHeight, outStride, bufferHeight, outUvOffset, bufLen);
                    loggedFallback = true;
                }
                return true;
            }
        }
    }
    return false;
}

// Helper: drain output frames from decoder, handling format changes.
// Drains ALL available frames and returns the last one (most recent PTS),
// which avoids showing stale DPB frames that cause black flashing.
// Returns true if at least one frame was decoded.
static bool DrainDecoderOutput(IMFTransform* mft, uint32_t& width, uint32_t& height,
                                int32_t& yStride, GUID& subtype, bool& formatEstablished,
                                std::vector<uint8_t>& outRGBA,
                                uint32_t& outWidth, uint32_t& outHeight,
                                int& failCount, size_t inputLen) {
    bool gotFrame = false;
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = mft->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return false;

    DWORD baseBufSize = streamInfo.cbSize ? streamInfo.cbSize : 1920 * 1080 * 3 / 2;

    for (;;) {
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
            HandleStreamChange(mft, width, height, yStride, subtype, formatEstablished);
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

        // Extract decoded data — use IMF2DBuffer to get real GPU stride
        BYTE* data = nullptr;
        LONG realStride = static_cast<LONG>(width);
        IMFMediaBuffer* lockedBuf = nullptr;
        IMF2DBuffer* locked2d = nullptr;

        // Try IMF2DBuffer first (preserves GPU stride)
        IMFMediaBuffer* rawBuf = nullptr;
        if (SUCCEEDED(outputBuffer.pSample->GetBufferByIndex(0, &rawBuf))) {
            IMF2DBuffer* buf2d = nullptr;
            if (SUCCEEDED(rawBuf->QueryInterface(IID_PPV_ARGS(&buf2d)))) {
                hr = buf2d->Lock2D(&data, &realStride);
                if (SUCCEEDED(hr)) {
                    lockedBuf = rawBuf;
                    locked2d = buf2d;
                } else {
                    buf2d->Release();
                    rawBuf->Release();
                    rawBuf = nullptr;
                }
            } else {
                rawBuf->Release();
                rawBuf = nullptr;
            }
        }

        // Fallback: contiguous buffer
        if (!locked2d) {
            hr = outputBuffer.pSample->ConvertToContiguousBuffer(&lockedBuf);
            if (SUCCEEDED(hr)) {
                DWORD maxLen = 0, curLen = 0;
                hr = lockedBuf->Lock(&data, &maxLen, &curLen);
                if (FAILED(hr)) {
                    lockedBuf->Release();
                    lockedBuf = nullptr;
                }
            }
        }

        if (lockedBuf && data) {
            int32_t actualStride = static_cast<int32_t>(realStride);
            size_t uvOffset = 0;  // 0 = use default (tightly-packed)
            uint32_t convWidth = width;
            uint32_t convHeight = height;

            // When buffer size doesn't match expected, compute real buffer layout.
            // Content dimensions from media type (SPS) may differ from actual
            // buffer dimensions due to encoder padding or surface pool sizing.
            if (!locked2d && subtype == MFVideoFormat_NV12) {
                DWORD bufLen = 0;
                lockedBuf->GetCurrentLength(&bufLen);
                if (ComputeNv12BufferLayout(bufLen, width, height, actualStride, uvOffset)) {
                    // Clamp content to actual buffer — prevents OOB reads when
                    // SPS reports larger dimensions than the decoded picture
                    if (convWidth > static_cast<uint32_t>(actualStride))
                        convWidth = static_cast<uint32_t>(actualStride);
                }
            }

            if (!formatEstablished) {
                HandleStreamChange(mft, width, height, yStride, subtype, formatEstablished);
            }

            if (subtype == MFVideoFormat_YV12 || subtype == MFVideoFormat_IYUV) {
                Yv12ToBgra(data, convWidth, convHeight, actualStride, outRGBA);
            } else if (subtype == MFVideoFormat_YUY2) {
                Yuy2ToBgra(data, convWidth, convHeight, actualStride, outRGBA);
            } else {
                Nv12ToBgra(data, convWidth, convHeight, actualStride, outRGBA, uvOffset);
            }
            outWidth = convWidth;
            outHeight = convHeight;
            gotFrame = true;  // keep overwriting — last frame wins

            if (locked2d) {
                locked2d->Unlock2D();
                locked2d->Release();
            } else {
                lockedBuf->Unlock();
            }
            lockedBuf->Release();
        } else if (locked2d) {
            locked2d->Release();
        }

        outSample->Release();
    }

    return gotFrame && !outRGBA.empty();
}

bool MfVideoDecoder::DecodeFrame(const uint8_t* bitstream, size_t len,
                                   std::vector<uint8_t>& outRGBA,
                                   uint32_t& outWidth, uint32_t& outHeight) {
    if (!m_impl->initialized || !m_impl->mft || len == 0) return false;
    static int failCount = 0;

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
                          m_impl->yStride, m_impl->outputSubtype,
                          m_impl->formatEstablished,
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
                              m_impl->yStride, m_impl->outputSubtype,
                              m_impl->formatEstablished,
                              outRGBA, outWidth, outHeight, failCount, len);
}
