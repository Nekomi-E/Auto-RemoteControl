#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include <atomic>

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
                    uint32_t fps, bool enableAudio);
    void Stop();

    void SubmitVideoFrame(const std::vector<uint8_t>& rawData,
                          uint32_t width, uint32_t height,
                          int64_t timestampMs);
    void SubmitAudioFrame(const std::vector<uint8_t>& rawData, int64_t timestampMs);

    bool GetEncodedVideoFrame(EncodedFrame& outFrame, int timeoutMs);
    bool GetEncodedAudioFrame(EncodedFrame& outFrame, int timeoutMs);

    void AdjustBitrate(uint32_t bitrate);
    void RequestKeyFrame();

    uint32_t GetEncoderWidth() const;
    uint32_t GetEncoderHeight() const;

    Stats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
