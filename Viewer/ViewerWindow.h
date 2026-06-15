#pragma once
#include <windows.h>
#include <cstdint>

class ViewerSession;

class ViewerWindow {
public:
    ViewerWindow();
    ~ViewerWindow();

    bool Create(HINSTANCE hInstance, uint32_t width, uint32_t height, bool fullscreen);
    HWND GetHWND() const { return m_hwnd; }

    void ProcessPendingEvents(ViewerSession& session);
    void SetInputActive(bool active);

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void RegisterRawInput();

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_fullscreen = false;
    bool m_inputActive = false;
};
