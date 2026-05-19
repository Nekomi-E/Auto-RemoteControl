#include "ViewerWindow.h"
#include "ViewerSession.h"
#include "Common/Utils/Logger.h"
#include <d3d11.h>

// Forward declare the global session pointer from ViewerMain
extern ViewerSession* g_session;

ViewerWindow::ViewerWindow() {}

ViewerWindow::~ViewerWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool ViewerWindow::Create(HINSTANCE hInstance, uint32_t width, uint32_t height, bool fullscreen) {
    m_hInstance = hInstance;
    m_width = width;
    m_height = height;
    m_fullscreen = fullscreen;

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RemoteControlViewer";

    if (!RegisterClassEx(&wc)) {
        LOG_ERROR("Failed to register window class: %d", GetLastError());
        return false;
    }

    // Calculate window size to fit within work area
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    uint32_t availW = workArea.right - workArea.left;
    uint32_t availH = workArea.bottom - workArea.top;

    uint32_t clientW = width;
    uint32_t clientH = height;
    // Scale down if remote is larger than available space (leave 5% margin)
    if (clientW > availW || clientH > availH) {
        float scale = (float)availW / clientW;
        if ((float)availH / clientH < scale) scale = (float)availH / clientH;
        clientW = (uint32_t)(clientW * scale * 0.95f);
        clientH = (uint32_t)(clientH * scale * 0.95f);
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect = { 0, 0, (LONG)clientW, (LONG)clientH };
    if (fullscreen) {
        style = WS_POPUP;
        rect = { 0, 0, (LONG)availW, (LONG)availH };
    }
    AdjustWindowRect(&rect, style, FALSE);

    m_hwnd = CreateWindowEx(0, L"RemoteControlViewer",
                             L"RemoteControl - Viewer",
                             style,
                             fullscreen ? 0 : CW_USEDEFAULT,
                             fullscreen ? 0 : CW_USEDEFAULT,
                             rect.right - rect.left,
                             rect.bottom - rect.top,
                             nullptr, nullptr, hInstance, this);

    if (!m_hwnd) {
        LOG_ERROR("Failed to create window: %d", GetLastError());
        return false;
    }

    RegisterRawInput();

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    LOG_INFO("Viewer window created: %ux%u", width, height);
    return true;
}

void ViewerWindow::RegisterRawInput() {
    RAWINPUTDEVICE rid[2];

    // Mouse
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = m_hwnd;

    // Keyboard
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = m_hwnd;

    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
}

void ViewerWindow::SetInputActive(bool active) {
    m_inputActive = active;
    if (active) {
        SetWindowTextW(m_hwnd, L"RemoteControl - Viewer [INPUT ACTIVE - Scroll Lock to release]");
        // Capture cursor
        SetCapture(m_hwnd);
        ShowCursor(FALSE);
        // Clip cursor to window
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        ClientToScreen(m_hwnd, (POINT*)&rect.left);
        ClientToScreen(m_hwnd, (POINT*)&rect.right);
        ClipCursor(&rect);
    } else {
        SetWindowTextW(m_hwnd, L"RemoteControl - Viewer");
        ReleaseCapture();
        ShowCursor(TRUE);
        ClipCursor(nullptr);
    }
}

void ViewerWindow::ProcessPendingEvents(ViewerSession& session) {
    // Check for Scroll Lock toggle to activate/deactivate input mode
    static bool lastScrollLock = false;
    bool scrollLock = (GetKeyState(VK_SCROLL) & 1) != 0;

    if (scrollLock != lastScrollLock) {
        lastScrollLock = scrollLock;
        SetInputActive(scrollLock);
    }
}

LRESULT CALLBACK ViewerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ViewerWindow* self = nullptr;

    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        self = (ViewerWindow*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (ViewerWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        if (self) {
            self->m_width = LOWORD(lParam);
            self->m_height = HIWORD(lParam);
            if (g_session) {
                g_session->OnResize(self->m_width, self->m_height);
            }
        }
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (self && self->m_inputActive && g_session) {
            g_session->OnKeyEvent(msg, wParam, lParam);
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        if (self && self->m_inputActive && g_session) {
            g_session->OnMouseEvent(msg, wParam, lParam);
            return 0;
        }
        break;

    case WM_INPUT:
        if (self && self->m_inputActive && g_session) {
            g_session->OnRawInput((HRAWINPUT)lParam);
        }
        break;

    case WM_KILLFOCUS:
        if (self) {
            self->SetInputActive(false);
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
