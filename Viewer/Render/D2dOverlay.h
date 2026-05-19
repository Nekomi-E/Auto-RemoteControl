#pragma once
#include <windows.h>
#include <string>

struct ID2D1DeviceContext;
struct ID2D1SolidColorBrush;
struct IDWriteTextFormat;

class D2dOverlay {
public:
    D2dOverlay();
    ~D2dOverlay();

    bool Initialize(HWND hwnd, ID2D1DeviceContext* d2dContext);
    void Shutdown();
    void Draw(float fps, bool connected);

private:
    void CreateBrushes();
    void DrawText(const std::wstring& text, float x, float y, float fontSize,
                  ID2D1SolidColorBrush* brush);

    ID2D1DeviceContext* m_d2dContext = nullptr;
    ID2D1SolidColorBrush* m_greenBrush = nullptr;
    ID2D1SolidColorBrush* m_redBrush = nullptr;
    ID2D1SolidColorBrush* m_whiteBrush = nullptr;
    ID2D1SolidColorBrush* m_bgBrush = nullptr;
    IDWriteTextFormat* m_textFormat = nullptr;
    HWND m_targetHwnd = nullptr;
    bool m_initialized = false;
};
