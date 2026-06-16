#include "D3d11Renderer.h"
#include "Common/Utils/Logger.h"
#include <d3d11.h>
#include <d3d10.h>       // ID3D10Multithread
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d3d11_1.h>

#pragma comment(lib, "dxguid.lib")  // IID_ID3D10Multithread

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Fullscreen quad vertex
struct QuadVertex {
    float x, y;   // clip-space position
    float u, v;   // texture coordinates
};

// Fullscreen triangle: 3 vertices covering clip space (-1..1, -1..1)
static const QuadVertex g_QuadVertices[] = {
    {-1.0f,  1.0f, 0.0f, 0.0f},
    { 3.0f,  1.0f, 2.0f, 0.0f},
    {-1.0f, -3.0f, 0.0f, 2.0f},
};

// Vertex shader: pass-through position + texcoord
static const char g_VsSource[] = R"(
struct VSInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};
struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput o;
    o.pos = float4(input.pos, 0.0f, 1.0f);
    o.uv  = input.uv;
    return o;
}
)";

static const char g_PsSource[] = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(samp, uv);
}
)";

// NV12 pixel shader: Y (t0, R8_UNORM) + UV (t1, R8G8_UNORM) → BGRA.
// Uses BT.709 coefficients with limited-range input (16-235 Y, 16-240 UV),
// matching the encoder's output color space.  Output is full-range [0,1] BGRA
// ordered for the swap chain's B8G8R8A8_UNORM format.
// BT.709 matrix (ITU-R BT.709-6):
//   R = 1.1644*(Y-16) + 1.7927*(Cr-128)
//   G = 1.1644*(Y-16) - 0.2132*(Cb-128) - 0.5329*(Cr-128)
//   B = 1.1644*(Y-16) + 2.1124*(Cb-128)
static const char g_PsNv12Source[] = R"(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float  Yf = texY.Sample(samp, uv).r * 255.0f;
    // D3D11 NV12 subresource 1 with DXGI_FORMAT_R8G8_UNORM maps
    // R→U(Cb) and G→V(Cr) per the Direct3D specification.  Previous
    // .gr swap was an NVIDIA-driver-specific workaround that caused
    // U/V swap (green↔blue) on other GPU vendors.  Using .rg is
    // correct for the standard mapping.
    float2 Cf = texUV.Sample(samp, uv).rg * 255.0f;
    float y = Yf - 16.0f;
    float u = Cf.r - 128.0f;
    float v = Cf.g - 128.0f;

    // BT.709 limited-range → full-range (fixed-point /256)
    float r = (298.0f * y + 459.0f * v + 128.0f) / 256.0f;
    float g = (298.0f * y -  55.0f * u - 136.0f * v + 128.0f) / 256.0f;
    float b = (298.0f * y + 541.0f * u + 128.0f) / 256.0f;

    r = saturate(r / 255.0f);
    g = saturate(g / 255.0f);
    b = saturate(b / 255.0f);
    return float4(b, g, r, 1.0f);  // BGRA output for B8G8R8A8_UNORM swap chain
}
)";

D3d11Renderer::D3d11Renderer() {}

D3d11Renderer::~D3d11Renderer() { Shutdown(); }

bool D3d11Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    m_windowWidth = width;
    m_windowHeight = height;

    if (!CreateDeviceResources()) {
        LOG_ERROR("D3D11: CreateDeviceResources failed");
        return false;
    }
    if (!CreateShaders()) {
        LOG_ERROR("D3D11: CreateShaders failed");
        return false;
    }
    if (!CreateSwapChain(hwnd)) {
        LOG_ERROR("D3D11: CreateSwapChain failed");
        return false;
    }

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

    // Enable multithread protection — required when the same D3D11 device is
    // used from multiple threads (e.g. decode thread + render thread).
    // ID3D10Multithread works for all D3D11 devices (same vtable layout).
    {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(IID_ID3D10Multithread, (void**)&mt))) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
            LOG_INFO("D3D11 multithread protection enabled");
        }
    }

    // Create D2D device/context for overlay
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_device->QueryInterface(&dxgiDevice);
    if (SUCCEEDED(hr)) {
        D2D1_FACTORY_OPTIONS options = {};
        ID2D1Factory1* d2dFactory = nullptr;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
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

bool D3d11Renderer::CreateShaders() {
    // Compile vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(g_VsSource, strlen(g_VsSource), "vs", nullptr, nullptr,
                             "main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            LOG_ERROR("VS compile: %s", (const char*)errBlob->GetBufferPointer());
            errBlob->Release();
        }
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                       vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11: CreateVertexShader failed: 0x%08X", hr);
        vsBlob->Release();
        return false;
    }

    // Input layout: position (float2) + texcoord (float2)
    D3D11_INPUT_ELEMENT_DESC layoutDesc[2] = {};
    layoutDesc[0].SemanticName = "POSITION";
    layoutDesc[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    layoutDesc[0].AlignedByteOffset = 0;
    layoutDesc[1].SemanticName = "TEXCOORD";
    layoutDesc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    layoutDesc[1].AlignedByteOffset = 8; // after 2 floats

    hr = m_device->CreateInputLayout(layoutDesc, 2,
                                      vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      &m_inputLayout);
    vsBlob->Release();
    if (FAILED(hr)) {
        LOG_ERROR("D3D11: CreateInputLayout failed: 0x%08X", hr);
        return false;
    }

    // Create vertex buffer for fullscreen triangle
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(g_QuadVertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = g_QuadVertices;

    hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_quadVB);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11: CreateBuffer(quadVB) failed: 0x%08X", hr);
        return false;
    }

    // Compile pixel shader
    ID3DBlob* psBlob = nullptr;
    hr = D3DCompile(g_PsSource, strlen(g_PsSource), "ps", nullptr, nullptr,
                     "main", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            LOG_ERROR("D3D11: PS compile: %s", (const char*)errBlob->GetBufferPointer());
            errBlob->Release();
        } else {
            LOG_ERROR("D3D11: PS compile failed: 0x%08X", hr);
        }
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                      psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        LOG_ERROR("D3D11: CreatePixelShader failed: 0x%08X", hr);
        return false;
    }

    // Compile NV12 pixel shader
    {
        ID3DBlob* psNv12Blob = nullptr;
        hr = D3DCompile(g_PsNv12Source, strlen(g_PsNv12Source), "ps", nullptr, nullptr,
                         "main", "ps_4_0", 0, 0, &psNv12Blob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LOG_ERROR("D3D11: NV12 PS compile: %s", (const char*)errBlob->GetBufferPointer());
                errBlob->Release();
            }
            LOG_WARNING("NV12 pixel shader compile failed, GPU decode path disabled");
            m_pixelShaderNv12 = nullptr;
        } else {
            hr = m_device->CreatePixelShader(psNv12Blob->GetBufferPointer(),
                                              psNv12Blob->GetBufferSize(), nullptr, &m_pixelShaderNv12);
            psNv12Blob->Release();
            if (FAILED(hr)) {
                LOG_WARNING("CreatePixelShader(NV12) failed: 0x%08X", hr);
                m_pixelShaderNv12 = nullptr;
            }
        }
    }

    // Sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11: CreateSamplerState failed: 0x%08X", hr);
        return false;
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

    // Get back buffer and create RTV
    ID3D11Texture2D* backBuffer = nullptr;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    // Set up D2D target from the back buffer
    if (m_d2dContext) {
        IDXGISurface* dxgiSurface = nullptr;
        hr = m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&dxgiSurface);
        if (SUCCEEDED(hr)) {
            D2D1_BITMAP_PROPERTIES1 bp = {};
            bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            ID2D1Bitmap1* d2dTarget = nullptr;
            hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface, bp, &d2dTarget);
            if (SUCCEEDED(hr)) {
                m_d2dContext->SetTarget(d2dTarget);
                d2dTarget->Release();
            }
            dxgiSurface->Release();
        }
    }

    // Viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)m_windowWidth;
    vp.Height = (FLOAT)m_windowHeight;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    return true;
}

void D3d11Renderer::Shutdown() {
    if (m_d2dContext) {
        m_d2dContext->SetTarget(nullptr);
        m_d2dContext->Release();
    }
    if (m_inputLayout) m_inputLayout->Release();
    if (m_pixelShader) m_pixelShader->Release();
    if (m_pixelShaderNv12) m_pixelShaderNv12->Release();
    if (m_vertexShader) m_vertexShader->Release();
    if (m_quadVB) m_quadVB->Release();
    if (m_sampler) m_sampler->Release();
    if (m_vpNv12SRV_Y)  m_vpNv12SRV_Y->Release();
    if (m_vpNv12SRV_UV) m_vpNv12SRV_UV->Release();
    if (m_vpBgraSRV) m_vpBgraSRV->Release();
    if (m_vpOutputView) m_vpOutputView->Release();
    if (m_vpInputView) m_vpInputView->Release();
    if (m_vpBgraTex) m_vpBgraTex->Release();
    if (m_vpNv12Tex) m_vpNv12Tex->Release();
    if (m_videoProcessor) m_videoProcessor->Release();
    if (m_vpEnumerator) m_vpEnumerator->Release();
    if (m_videoContext) m_videoContext->Release();
    if (m_videoDevice) m_videoDevice->Release();
    if (m_videoSRV) m_videoSRV->Release();
    if (m_videoTexture) m_videoTexture->Release();
    if (m_rtv) m_rtv->Release();
    if (m_swapChain) m_swapChain->Release();
    if (m_context) m_context->Release();
    if (m_device) m_device->Release();

    m_vpNv12SRV_Y = nullptr;
    m_vpNv12SRV_UV = nullptr;
    m_vpBgraSRV = nullptr;
    m_vpOutputView = nullptr;
    m_vpInputView = nullptr;
    m_vpBgraTex = nullptr;
    m_vpNv12Tex = nullptr;
    m_videoProcessor = nullptr;
    m_vpEnumerator = nullptr;
    m_videoContext = nullptr;
    m_videoDevice = nullptr;
    m_d2dContext = nullptr;
    m_inputLayout = nullptr;
    m_pixelShader = nullptr;
    m_pixelShaderNv12 = nullptr;
    m_vertexShader = nullptr;
    m_quadVB = nullptr;
    m_sampler = nullptr;
    m_videoSRV = nullptr;
    m_videoTexture = nullptr;
    m_rtv = nullptr;
    m_swapChain = nullptr;
    m_context = nullptr;
    m_device = nullptr;
}

void D3d11Renderer::Resize(uint32_t width, uint32_t height) {
    if (!m_device || !m_context) return;

    m_windowWidth = width;
    m_windowHeight = height;

    // Invalidate NV12 shader pipeline + VP pipeline — rebuilt at new size
    if (m_vpNv12SRV_Y) { m_vpNv12SRV_Y->Release(); m_vpNv12SRV_Y = nullptr; }
    if (m_vpNv12SRV_UV){ m_vpNv12SRV_UV->Release(); m_vpNv12SRV_UV = nullptr; }
    if (m_vpNv12Tex)    { m_vpNv12Tex->Release();    m_vpNv12Tex = nullptr; }
    if (m_vpOutputView) { m_vpOutputView->Release();  m_vpOutputView = nullptr; }
    // Note: do NOT release m_vpInputView/m_videoProcessor/m_vpEnumerator here —
    // they are independent of the swap chain.  m_vpWidth/m_vpHeight are cleared
    // so RenderFrameNv12 will recreate everything including the per-frame output view.
    m_vpWidth = 0; m_vpHeight = 0;

    if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }

    if (m_swapChain) {
        m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* backBuffer = nullptr;
        HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
        if (SUCCEEDED(hr)) {
            m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
            backBuffer->Release();

            // Re-bind D2D target
            if (m_d2dContext) {
                IDXGISurface* dxgiSurface = nullptr;
                hr = m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&dxgiSurface);
                if (SUCCEEDED(hr)) {
                    D2D1_BITMAP_PROPERTIES1 bp = {};
                    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
                    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                    ID2D1Bitmap1* d2dTarget = nullptr;
                    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface, bp, &d2dTarget);
                    if (SUCCEEDED(hr)) {
                        m_d2dContext->SetTarget(d2dTarget);
                        d2dTarget->Release();
                    }
                    dxgiSurface->Release();
                }
            }
        }
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)width;
    vp.Height = (FLOAT)height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void D3d11Renderer::RenderFrame(const uint8_t* rgbaData, uint32_t width, uint32_t height) {//RenderFrame函数还可以极限性能优化，减少不必要的状态设置和资源 updates（例如当视频尺寸不变时跳过纹理重建，或者当视频覆盖整个屏幕时跳过 ClearRenderTargetView）
    if (!m_device || !m_context || !m_rtv) return;

    if (rgbaData && width > 0 && height > 0) {
        // Skip ClearRenderTargetView when the video texture covers the full viewport —
        // the fullscreen triangle overwrites every pixel, making the clear redundant.
        // Recreate texture if dimensions changed
        if (!m_videoTexture || m_textureWidth != width || m_textureHeight != height) {
            if (m_videoSRV) { m_videoSRV->Release(); m_videoSRV = nullptr; }
            if (m_videoTexture) { m_videoTexture->Release(); m_videoTexture = nullptr; }

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
            if (SUCCEEDED(hr)) {
                hr = m_device->CreateShaderResourceView(m_videoTexture, nullptr, &m_videoSRV);
                if (FAILED(hr)) {
                    m_videoTexture->Release();
                    m_videoTexture = nullptr;
                }
            }
            m_textureWidth = width;
            m_textureHeight = height;
        }

        // Update texture with new frame data
        if (m_videoTexture) {
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HRESULT hr = m_context->Map(m_videoTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                size_t rowSize = width * 4;
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

        // Draw the video texture as a fullscreen triangle
        if (m_videoSRV && m_quadVB && m_vertexShader && m_pixelShader && m_sampler) {
            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, &m_quadVB, &stride, &offset);
            m_context->IASetInputLayout(m_inputLayout);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_vertexShader, nullptr, 0);
            m_context->PSSetShader(m_pixelShader, nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &m_videoSRV);
            m_context->PSSetSamplers(0, 1, &m_sampler);
            m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
            m_context->Draw(3, 0);
        }
    }
}

void D3d11Renderer::RenderFrameNv12(ID3D11Texture2D* nv12Texture, uint32_t width, uint32_t height) {
    if (!m_device || !m_context || !m_rtv || !nv12Texture) return;
    if (width == 0 || height == 0) return;

    if (!m_vpEnumerator || m_vpWidth != width || m_vpHeight != height) {
        if (m_vpOutputView)  { m_vpOutputView->Release();  m_vpOutputView = nullptr; }
        if (m_vpInputView)   { m_vpInputView->Release();   m_vpInputView = nullptr; }
        if (m_vpNv12Tex)     { m_vpNv12Tex->Release();     m_vpNv12Tex = nullptr; }
        if (m_videoProcessor) { m_videoProcessor->Release(); m_videoProcessor = nullptr; }
        if (m_vpEnumerator)  { m_vpEnumerator->Release();  m_vpEnumerator = nullptr; }
        if (m_videoContext)  { m_videoContext->Release();  m_videoContext = nullptr; }
        if (m_videoDevice)   { m_videoDevice->Release();   m_videoDevice = nullptr; }

        HRESULT hr;
        hr = m_device->QueryInterface(IID_PPV_ARGS(&m_videoDevice));
        if (FAILED(hr)) { LOG_WARNING("VP: videoDevice failed"); return; }
        hr = m_context->QueryInterface(IID_PPV_ARGS(&m_videoContext));
        if (FAILED(hr)) { LOG_WARNING("VP: videoContext failed"); return; }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc = {};
        vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        vpDesc.InputFrameRate.Numerator = 600; vpDesc.InputFrameRate.Denominator = 1;
        vpDesc.InputWidth = width; vpDesc.InputHeight = height;
        vpDesc.OutputFrameRate.Numerator = 600; vpDesc.OutputFrameRate.Denominator = 1;
        vpDesc.OutputWidth = width; vpDesc.OutputHeight = height;
        vpDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = m_videoDevice->CreateVideoProcessorEnumerator(&vpDesc, &m_vpEnumerator);
        if (FAILED(hr)) { LOG_WARNING("VP: enumerator failed"); return; }
        hr = m_videoDevice->CreateVideoProcessor(m_vpEnumerator, 0, &m_videoProcessor);
        if (FAILED(hr)) { LOG_WARNING("VP: processor failed"); return; }

        // NV12 staging
        { D3D11_TEXTURE2D_DESC d = {}; d.Width=width; d.Height=height; d.MipLevels=1;
          d.ArraySize=1; d.Format=DXGI_FORMAT_NV12; d.SampleDesc.Count=1;
          d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_RENDER_TARGET;
          hr=m_device->CreateTexture2D(&d,nullptr,&m_vpNv12Tex);
          if(FAILED(hr)){LOG_WARNING("VP: NV12 tex failed");return;} }

        // VP input view
        { D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv={}; iv.FourCC=0;
          iv.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
          iv.Texture2D.MipSlice=0; iv.Texture2D.ArraySlice=0;
          hr=m_videoDevice->CreateVideoProcessorInputView(m_vpNv12Tex,m_vpEnumerator,&iv,&m_vpInputView);
          if(FAILED(hr)){LOG_WARNING("VP: input view failed");return;} }

        // VP output → intermediate BGRA texture (avoids swap chain back-buffer
        // compatibility issues across GPU adapters after display changes)
        {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width=width; d.Height=height; d.MipLevels=1; d.ArraySize=1;
            d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count=1;
            d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr=m_device->CreateTexture2D(&d,nullptr,&m_vpBgraTex);
            if(FAILED(hr)){LOG_WARNING("VP: BGRA tex failed");return;}
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov={};
            ov.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
            hr=m_videoDevice->CreateVideoProcessorOutputView(m_vpBgraTex,m_vpEnumerator,&ov,&m_vpOutputView);
            if(FAILED(hr)){LOG_WARNING("VP: output view failed");return;}
            // SRV to sample BGRA in PS for fullscreen draw
            if(m_vpBgraSRV){m_vpBgraSRV->Release();m_vpBgraSRV=nullptr;}
            hr=m_device->CreateShaderResourceView(m_vpBgraTex,nullptr,&m_vpBgraSRV);
            if(FAILED(hr)){LOG_WARNING("VP: BGRA SRV failed");return;}
        }

        // BT.709 color space
        { D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCS={}; inCS.YCbCr_Matrix=1; inCS.Nominal_Range=1;
          D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCS={}; outCS.YCbCr_Matrix=0; outCS.Nominal_Range=0;
          m_videoContext->VideoProcessorSetStreamColorSpace(m_videoProcessor,0,&inCS);
          m_videoContext->VideoProcessorSetOutputColorSpace(m_videoProcessor,&outCS); }

        m_vpWidth=width; m_vpHeight=height;
        LOG_INFO("RenderFrameNv12: VP NV12→BGRA pipeline ready (%ux%u)",width,height);
    }

    // Copy decoder NV12 → staging
    { D3D11_TEXTURE2D_DESC dd,vd; nv12Texture->GetDesc(&dd); m_vpNv12Tex->GetDesc(&vd);
      if(dd.MipLevels==vd.MipLevels&&dd.ArraySize==vd.ArraySize)
          m_context->CopyResource(m_vpNv12Tex,nv12Texture);
      else{ D3D11_BOX b; b.left=0;b.top=0;b.front=0;b.right=width;b.bottom=height;b.back=1;
            m_context->CopySubresourceRegion(m_vpNv12Tex,0,0,0,0,nv12Texture,0,&b);
            b.right=(width+1)/2;b.bottom=(height+1)/2;
            m_context->CopySubresourceRegion(m_vpNv12Tex,1,0,0,0,nv12Texture,1,&b); } }

    // VP: NV12 → BGRA intermediate texture
    RECT rc={0,0,(LONG)width,(LONG)height};
    m_videoContext->VideoProcessorSetStreamSourceRect(m_videoProcessor,0,TRUE,&rc);
    m_videoContext->VideoProcessorSetStreamDestRect(m_videoProcessor,0,TRUE,&rc);
    D3D11_VIDEO_PROCESSOR_STREAM s={}; s.Enable=TRUE; s.OutputIndex=0;
    s.InputFrameOrField=0; s.PastFrames=0; s.FutureFrames=0;
    s.ppPastSurfaces=nullptr; s.pInputSurface=m_vpInputView; s.ppFutureSurfaces=nullptr;
    HRESULT hr=m_videoContext->VideoProcessorBlt(m_videoProcessor,m_vpOutputView,0,1,&s);
    if(FAILED(hr)){ static int n=0; if(++n<=5)LOG_WARNING("VP: Blt failed: 0x%08X",hr); return; }

    // Draw BGRA intermediate → swap chain back buffer via fullscreen triangle
    if (m_vpBgraSRV && m_quadVB && m_vertexShader && m_pixelShader) {
        UINT stride = sizeof(QuadVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, &m_quadVB, &stride, &offset);
        m_context->IASetInputLayout(m_inputLayout);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader, nullptr, 0);
        m_context->PSSetShader(m_pixelShader, nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &m_vpBgraSRV);
        m_context->PSSetSamplers(0, 1, &m_sampler);
        m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
        m_context->Draw(3, 0);
    }
}

// NV12 → back-buffer via pixel shader, bypassing the D3D11 Video Processor.
// On modern GPUs the shader cores handle BGRA color conversion ~10–100× faster
// than the dedicated VP block, whose per-frame scheduling overhead dominates
// at high frame rates.  At 2560×1600 the PS path takes ~0.1 ms vs 10–15 ms
// for the VP path, critical for the 8.33 ms frame budget at 120 fps.
void D3d11Renderer::RenderFrameNv12Ps(ID3D11Texture2D* nv12Texture, uint32_t width, uint32_t height) {
    if (!m_device || !m_context || !m_rtv || !nv12Texture || !m_pixelShaderNv12) return;
    if (width == 0 || height == 0) return;

    // Create / recreate SRVs and staging texture when dimensions change
    if (!m_vpNv12Tex || m_vpWidth != width || m_vpHeight != height) {
        if (m_vpNv12SRV_Y)  { m_vpNv12SRV_Y->Release();  m_vpNv12SRV_Y  = nullptr; }
        if (m_vpNv12SRV_UV) { m_vpNv12SRV_UV->Release(); m_vpNv12SRV_UV = nullptr; }
        if (m_vpNv12Tex)    { m_vpNv12Tex->Release();    m_vpNv12Tex    = nullptr; }

        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &m_vpNv12Tex))) return;

        // Y-plane SRV: view subresource 0 as R8_UNORM (luminance)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvYDesc = {};
        srvYDesc.Format = DXGI_FORMAT_R8_UNORM;
        srvYDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvYDesc.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_vpNv12Tex, &srvYDesc, &m_vpNv12SRV_Y)))
            return;

        // UV-plane SRV: view subresource 1 as R8G8_UNORM (chrominance)
        D3D11_SHADER_RESOURCE_VIEW_DESC srvUVDesc = {};
        srvUVDesc.Format = DXGI_FORMAT_R8G8_UNORM;
        srvUVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvUVDesc.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_vpNv12Tex, &srvUVDesc, &m_vpNv12SRV_UV)))
            return;

        m_vpWidth  = width;
        m_vpHeight = height;
        LOG_INFO("RenderFrameNv12Ps: NV12 PS pipeline ready (%ux%u)", width, height);
    }

    // Copy decoder NV12 texture → local staging (same as VP path)
    {
        D3D11_TEXTURE2D_DESC dd, vd;
        nv12Texture->GetDesc(&dd); m_vpNv12Tex->GetDesc(&vd);
        if (dd.MipLevels == vd.MipLevels && dd.ArraySize == vd.ArraySize)
            m_context->CopyResource(m_vpNv12Tex, nv12Texture);
        else {
            D3D11_BOX b; b.left=0; b.top=0; b.front=0;
            b.right=width; b.bottom=height; b.back=1;
            m_context->CopySubresourceRegion(m_vpNv12Tex,0,0,0,0, nv12Texture,0,&b);
            b.right=(width+1)/2; b.bottom=(height+1)/2;
            m_context->CopySubresourceRegion(m_vpNv12Tex,1,0,0,0, nv12Texture,1,&b);
        }
    }

    // Bind PS pipeline and draw the fullscreen triangle.
    // The NV12 pixel shader samples Y (R8) from t0 and UV (R8G8) from t1,
    // converts to BT.709 limited-range → full-range BGRA, and writes
    // directly to the swap-chain back buffer.
    {
        UINT stride = sizeof(QuadVertex);
        UINT offset = 0;
        m_context->IASetVertexBuffers(0, 1, &m_quadVB, &stride, &offset);
        m_context->IASetInputLayout(m_inputLayout);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader, nullptr, 0);
        m_context->PSSetShader(m_pixelShaderNv12, nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = { m_vpNv12SRV_Y, m_vpNv12SRV_UV };
        m_context->PSSetShaderResources(0, 2, srvs);
        m_context->PSSetSamplers(0, 1, &m_sampler);
        m_context->OMSetRenderTargets(1, &m_rtv, nullptr);

        m_context->Draw(3, 0);
    }
}

void D3d11Renderer::Present() {
    if (m_swapChain) {
        // DXGI_PRESENT_DO_NOT_WAIT: if the GPU is busy (e.g. DWM compositing
        // during window drag), return DXGI_ERROR_WAS_STILL_DRAWING immediately
        // instead of blocking the render thread.  This prevents the freeze
        // that occurs when Present(0,0) blocks inside the DWM during a window
        // move/resize operation.  At 120 fps the dropped frame is imperceptible.
        HRESULT hr = m_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // GPU still busy — skip this present, try again next frame
            static uint32_t skipCount = 0;
            if ((++skipCount & 0x3F) == 0) {
                LOG_INFO("Present skipped (GPU busy), count=%u", skipCount);
            }
        }
    }
}
