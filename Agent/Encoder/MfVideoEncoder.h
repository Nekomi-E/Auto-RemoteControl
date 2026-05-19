#pragma once
#include <vector>
#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IMFActivate;

class MfVideoEncoder {
public:
    MfVideoEncoder();
    ~MfVideoEncoder();

    bool Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps,
                    ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dContext = nullptr);
    void Shutdown();

    bool EncodeFrame(const uint8_t* rawFrame, uint32_t width, uint32_t height,
                     std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetCodecType() const;  // 0=H.264, 1=HEVC

    void SetBitrate(uint32_t bitrate);
    void RequestKeyFrame();

private:
    struct Impl;

    static bool SetupEncoderCandidate(IMFActivate* activate, unsigned int idx,
                                      ID3D11Device* captureDevice,
                                      ID3D11DeviceContext* captureContext,
                                      Impl* impl);
    bool ConfigureMediaTypes();
    bool ProcessInput(const uint8_t* rawFrame, uint32_t width, uint32_t height);
    bool ProcessOutput(std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    std::unique_ptr<Impl> m_impl;
};
