#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include <atomic>

struct ID3D11Device;
struct ID3D11DeviceContext;

class EncoderManager {
public:
    struct EncodedFrame {
        std::vector<uint8_t> data;
        bool isKeyFrame = false;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t timestampMs = 0;
    };

    struct Stats {
        float encodeFps = 0;
        float currentBitrate = 0;
        uint32_t encodedFrames = 0;
        uint32_t keyFrames = 0;
    };

    EncoderManager();
    ~EncoderManager();

    bool Initialize(uint32_t width, uint32_t height, uint32_t bitrate,
                    uint32_t fps, bool enableAudio,
                    ID3D11Device* d3dDevice = nullptr,
                    ID3D11DeviceContext* d3dContext = nullptr);
    void Stop();

    void SubmitVideoFrame(std::vector<uint8_t> rawData,
                          uint32_t width, uint32_t height,
                          int64_t timestampMs);
    void SubmitAudioFrame(const std::vector<uint8_t>& rawData, int64_t timestampMs);

    bool GetEncodedVideoFrame(EncodedFrame& outFrame, int timeoutMs);
    bool GetEncodedAudioFrame(EncodedFrame& outFrame, int timeoutMs);

    void AdjustBitrate(uint32_t bitrate);
    void RequestKeyFrame();

    uint32_t GetEncoderWidth() const;
    uint32_t GetEncoderHeight() const;
    uint32_t GetCodecType() const;  // 0=H.264, 1=HEVC

    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
