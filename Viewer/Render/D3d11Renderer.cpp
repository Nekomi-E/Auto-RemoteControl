#include "D3d11Renderer.h"
#include "Common/Utils/Logger.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d3d11_1.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")

D3d11Renderer::D3d11Renderer() {}

D3d11Renderer::~D3d11Renderer() { Shutdown(); }

bool D3d11Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    m_windowWidth = width;
    m_windowHeight = height;

    if (!CreateDeviceResources()) return false;
    if (!CreateSwapChain(hwnd)) return false;

    LOG_INFO("D3D11 renderer initialized: %ux%u", width, height);
    return true;
}

bool D3d11Renderer::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                    featureLevels, ARRAYSIZE(featureLevels),
                                    D3D11_SDK_VERSION,
                                    &m_device, nullptr, &m_context);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Create D2D device/context for overlay
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_device->QueryInterface(&dxgiDevice);
    if (SUCCEEDED(hr)) {
        D2D1_FACTORY_OPTIONS options = {};
        ID2D1Factory1* d2dFactory = nullptr;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                IID_ID2D1Factory1, &options, (void**)&d2dFactory);
        if (SUCCEEDED(hr)) {
            ID2D1Device* d2dDevice = nullptr;
            hr = d2dFactory->CreateDevice(dxgiDevice, &d2dDevice);
            if (SUCCEEDED(hr)) {
                d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                               &m_d2dContext);
                d2dDevice->Release();
            }
            d2dFactory->Release();
        }
        dxgiDevice->Release();
    }

    return true;
}

bool D3d11Renderer::CreateSwapChain(HWND hwnd) {
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = m_device->QueryInterface(&dxgiDevice);
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = m_windowWidth;
    scDesc.Height = m_windowHeight;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
    fsDesc.Windowed = TRUE;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(m_device, hwnd, &scDesc, &fsDesc, nullptr, &sc1);
    factory->Release();

    if (FAILED(hr)) {
        LOG_ERROR("CreateSwapChainForHwnd failed: 0x%08X", hr);
        return false;
    }

    hr = sc1->QueryInterface(&m_swapChain);
    sc1->Release();
    if (FAILED(hr)) return false;

    // Get back buffer
    ID3D11Texture2D* backBuffer = nullptr;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)m_windowWidth;
    vp.Height = (FLOAT)m_windowHeight;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    return true;
}

void D3d11Renderer::Shutdown() {
    if (m_d2dContext) m_d2dContext->Release();
    if (m_videoSRV) m_videoSRV->Release();
    if (m_videoTexture) m_videoTexture->Release();
    if (m_rtv) m_rtv->Release();
    if (m_swapChain) m_swapChain->Release();
    if (m_context) m_context->Release();
    if (m_device) m_device->Release();

    m_d2dContext = nullptr;
    m_videoSRV = nullptr;
    m_videoTexture = nullptr;
    m_rtv = nullptr;
    m_swapChain = nullptr;
    m_context = nullptr;
    m_device = nullptr;
}

void D3d11Renderer::Resize(uint32_t width, uint32_t height) {
    m_windowWidth = width;
    m_windowHeight = height;

    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }

    if (m_swapChain) {
        m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* backBuffer = nullptr;
        m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
        if (backBuffer) {
            m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
            backBuffer->Release();
        }
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)width;
    vp.Height = (FLOAT)height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void D3d11Renderer::RenderFrame(const uint8_t* rgbaData, uint32_t width, uint32_t height) {
    if (!m_device || !m_context) return;

    // Clear to black
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv, clearColor);

    if (!rgbaData || width == 0 || height == 0) return;

    // Recreate texture if dimensions changed
    if (!m_videoTexture || m_textureWidth != width || m_textureHeight != height) {
        if (m_videoSRV) m_videoSRV->Release();
        if (m_videoTexture) m_videoTexture->Release();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_videoTexture);
        if (FAILED(hr)) return;

        hr = m_device->CreateShaderResourceView(m_videoTexture, nullptr, &m_videoSRV);
        if (FAILED(hr)) return;

        m_textureWidth = width;
        m_textureHeight = height;
    }

    // Update texture with new frame data
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_videoTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        size_t rowSize = width * 4;
        // BGRA input matches DXGI_FORMAT_B8G8R8A8_UNORM
        if (mapped.RowPitch == rowSize) {
            memcpy(mapped.pData, rgbaData, rowSize * height);
        } else {
            for (uint32_t row = 0; row < height; ++row) {
                memcpy(static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                       rgbaData + row * rowSize, rowSize);
            }
        }
        m_context->Unmap(m_videoTexture, 0);
    }
}

void D3d11Renderer::Present() {
    if (m_swapChain) {
        m_swapChain->Present(1, 0); // VSync on
    }
}

// Helper to create a D2D bitmap from the current video texture
// (used by D2dOverlay to render stats on top of video)
