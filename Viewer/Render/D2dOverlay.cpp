#include "D2dOverlay.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

D2dOverlay::D2dOverlay() {}

D2dOverlay::~D2dOverlay() { Shutdown(); }

bool D2dOverlay::Initialize(HWND hwnd, ID2D1DeviceContext* d2dContext) {
    m_targetHwnd = hwnd;
    m_d2dContext = d2dContext;
    if (!m_d2dContext) return false;

    CreateBrushes();

    // Create text format
    IDWriteFactory* dwFactory = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      (IUnknown**)&dwFactory);
    if (SUCCEEDED(hr)) {
        dwFactory->CreateTextFormat(L"Consolas", nullptr,
                                     DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL,
                                     16.0f, L"en-us", &m_textFormat);
        dwFactory->Release();
    }

    m_initialized = true;
    return true;
}

void D2dOverlay::CreateBrushes() {
    if (!m_d2dContext) return;

    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 1.0f, 0.0f, 0.8f), &m_greenBrush);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.8f), &m_redBrush);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f), &m_whiteBrush);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &m_bgBrush);
}

void D2dOverlay::Shutdown() {
    m_initialized = false;
    if (m_greenBrush) m_greenBrush->Release();
    if (m_redBrush) m_redBrush->Release();
    if (m_whiteBrush) m_whiteBrush->Release();
    if (m_bgBrush) m_bgBrush->Release();
    if (m_textFormat) m_textFormat->Release();
    m_greenBrush = nullptr;
    m_redBrush = nullptr;
    m_whiteBrush = nullptr;
    m_bgBrush = nullptr;
    m_textFormat = nullptr;
}

void D2dOverlay::Draw(float fps, bool connected) {
    if (!m_initialized || !m_d2dContext) return;

    m_d2dContext->BeginDraw();

    // Semi-transparent background bar at top
    D2D1_SIZE_F targetSize = m_d2dContext->GetSize();
    D2D1_RECT_F bgRect = D2D1::RectF(0, 0, targetSize.width, 28);

    if (m_bgBrush) {
        m_d2dContext->FillRectangle(bgRect, m_bgBrush);
    }

    // Connection status
    wchar_t statusText[128];
    swprintf(statusText, 128, L"%s  |  %.1f FPS  |  Scroll Lock: Input %s",
             connected ? L"[CONNECTED]" : L"[DISCONNECTED]",
             fps,
             (GetKeyState(VK_SCROLL) & 1) ? L"ON" : L"OFF");

    auto brush = connected ? m_greenBrush : m_redBrush;
    DrawText(statusText, 8, 4, 14.0f, brush);

    m_d2dContext->EndDraw();
}

void D2dOverlay::DrawText(const std::wstring& text, float x, float y,
                           float fontSize, ID2D1SolidColorBrush* brush) {
    if (!m_d2dContext || !m_textFormat || !brush) return;

    D2D1_SIZE_F targetSize = m_d2dContext->GetSize();
    D2D1_RECT_F rect = D2D1::RectF(x, y, targetSize.width, y + 24);
    m_d2dContext->DrawText(text.c_str(), static_cast<UINT32>(text.length()),
                            m_textFormat, rect, brush);
}
