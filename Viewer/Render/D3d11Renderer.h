#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11Buffer;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID2D1DeviceContext;

class D3d11Renderer {
public:
    D3d11Renderer();
    ~D3d11Renderer();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    void RenderFrame(const uint8_t* rgbaData, uint32_t width, uint32_t height);
    void Present();

    ID2D1DeviceContext* GetD2DDeviceContext() const { return m_d2dContext; }
    ID3D11Device* GetDevice() const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }

private:
    bool CreateDeviceResources();
    bool CreateSwapChain(HWND hwnd);
    bool CreateShaders();
    bool CreateFullscreenQuad();

    HWND m_hwnd = nullptr;
    uint32_t m_windowWidth = 0;
    uint32_t m_windowHeight = 0;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    ID3D11Texture2D* m_videoTexture = nullptr;
    ID3D11ShaderResourceView* m_videoSRV = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;

    // Fullscreen quad
    ID3D11Buffer* m_quadVB = nullptr;
    ID3D11VertexShader* m_vertexShader = nullptr;
    ID3D11PixelShader* m_pixelShader = nullptr;
    ID3D11InputLayout* m_inputLayout = nullptr;

    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;

    ID2D1DeviceContext* m_d2dContext = nullptr;
};
