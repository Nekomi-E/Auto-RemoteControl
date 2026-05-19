#include "Common/Common.h"
#include "Common/Utils/Logger.h"
#include "Common/Utils/Config.h"
#include "ViewerWindow.h"
#include "ViewerSession.h"
#include <d3d11.h>

// Global references for the window procedure
ViewerWindow* g_window = nullptr;
ViewerSession* g_session = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    (void)nCmdShow;

    // Declare DPI awareness so GetClientRect / SPI_GETWORKAREA return physical
    // pixels. Without this, on high-DPI displays the swap chain is created at
    // logical size and then stretched by DWM, causing double-scaling blur.
    SetProcessDPIAware();

    Logger::Instance().SetLevel(LogLevel::Debug);

    // Parse command line
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    std::vector<char*> argv;
    if (wargv) {
        for (int i = 0; i < argc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], len, nullptr, nullptr);
            args.push_back(s);
        }
        LocalFree(wargv);
        for (auto& a : args) argv.push_back(&a[0]);
    }

    auto config = Config::ParseViewerArgs(static_cast<int>(argv.size()), argv.data());

    if (config.password.empty()) {
        LOG_ERROR("Password is required. Use --password <password> to set one.");
        Config::PrintViewerHelp();
        return 1;
    }

    LOG_INFO("RemoteControl Viewer connecting to %s:%u", config.host.c_str(), config.port);

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("CoInitializeEx failed: 0x%08X", hr);
        return 1;
    }

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Create session
    ViewerSession session;
    if (!session.Initialize(config)) {
        LOG_ERROR("Failed to initialize Viewer session");
        CoUninitialize();
        return 1;
    }
    g_session = &session;

    // Get remote screen dimensions for window size
    uint32_t remoteWidth = session.GetRemoteWidth();
    uint32_t remoteHeight = session.GetRemoteHeight();

    // Create window
    ViewerWindow window;
    if (!window.Create(hInstance, remoteWidth, remoteHeight, config.fullscreen)) {
        LOG_ERROR("Failed to create window");
        session.Stop();
        CoUninitialize();
        return 1;
    }
    g_window = &window;

    // Set session render target
    if (!session.SetRenderWindow(window.GetHWND())) {
        LOG_ERROR("Failed to set render window");
        session.Stop();
        CoUninitialize();
        return 1;
    }

    // Start session threads
    session.Start();

    // Message loop
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // Render the latest frame
            session.RenderFrame();

            // Process window events
            window.ProcessPendingEvents(session);
        }
    }

    // Shutdown
    session.Stop();
    g_session = nullptr;
    g_window = nullptr;

    CoUninitialize();
    WSACleanup();

    return 0;
}
