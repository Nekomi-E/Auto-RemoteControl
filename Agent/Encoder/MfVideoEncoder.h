#pragma once
#include <vector>
#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IMFActivate;
struct IMFSample;

class MfVideoEncoder {
public:
    MfVideoEncoder();
    ~MfVideoEncoder();

    bool Initialize(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps,
                    uint32_t quality = 0,
                    ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dContext = nullptr);
    void Shutdown();

    bool EncodeFrame(const uint8_t* rawFrame, uint32_t width, uint32_t height,
                     std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    // GPU path: feed a captured BGRA texture directly to the encoder.
    // The texture is copied intra-GPU to the encoder's internal BGRA surface,
    // converted to NV12 via D3D11 VideoProcessor, and submitted to the MFT.
    // Returns false if GPU path is not available (caller should fall back).
    bool EncodeFrameGpu(ID3D11Texture2D* bgraTexture, uint32_t width, uint32_t height,
                        std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetCodecType() const;  // 0=H.264, 1=HEVC

    void SetBitrate(uint32_t bitrate);
    void RequestKeyFrame();

    // Expose D3D11 device for GPU texture sharing
    ID3D11Device* GetD3DDevice() const;

private:
    struct Impl;

    static bool SetupEncoderCandidate(IMFActivate* activate, unsigned int idx,
                                      ID3D11Device* captureDevice,
                                      ID3D11DeviceContext* captureContext,
                                      Impl* impl);
    static bool InitVideoProcessor(Impl* impl, uint32_t width, uint32_t height);
    static bool ConvertBgraToNv12Gpu(Impl* impl, const uint8_t* bgraData,
                                     uint32_t width, uint32_t height,
                                     IMFSample** outSample);
    static bool ConvertTextureToNv12Gpu(Impl* impl, ID3D11Texture2D* bgraTexture,
                                        uint32_t width, uint32_t height,
                                        IMFSample** outSample);
    bool ConfigureMediaTypes();
    bool TrySetInputType(uint32_t resW, uint32_t resH);
    bool TrySetOutputType(uint32_t resW, uint32_t resH);
    void FinalizeMediaTypes(uint32_t resW, uint32_t resH);
    bool ProcessInput(const uint8_t* rawFrame, uint32_t width, uint32_t height);
    bool ProcessOutput(std::vector<uint8_t>& outBitstream, bool& outIsKeyFrame);

    std::unique_ptr<Impl> m_impl;
};
