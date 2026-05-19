#include "RawInputCapture.h"
#include "Common/Utils/Timer.h"
#include <vector>

Protocol::InputEvent RawInputCapture::ProcessRawInput(HRAWINPUT hRawInput) {
    Protocol::InputEvent ev;
    ev.timestamp = Timer::NowMs();

    UINT size = 0;
    GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return ev;

    std::vector<BYTE> buf(size);
    GetRawInputData(hRawInput, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER));

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf.data());

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& mouse = raw->data.mouse;

        if (mouse.usFlags & MOUSE_MOVE_RELATIVE) {
            ev.type = Protocol::InputType::MOUSE_MOVE;
            ev.mouseMove = Protocol::MouseMoveEvent{
                static_cast<int16_t>(mouse.lLastX),
                static_cast<int16_t>(mouse.lLastY)
            };
        } else if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
            ev.type = Protocol::InputType::MOUSE_MOVE;
            ev.mouseMove = Protocol::MouseMoveEvent{
                static_cast<int16_t>(mouse.lLastX),
                static_cast<int16_t>(mouse.lLastY)
            };
        }
    }

    return ev;
}

Protocol::InputEvent RawInputCapture::ProcessKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
    Protocol::InputEvent ev;
    ev.timestamp = Timer::NowMs();

    bool isDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    ev.type = isDown ? Protocol::InputType::KEY_DOWN : Protocol::InputType::KEY_UP;

    Protocol::KeyEvent ke;
    ke.vkCode = static_cast<uint16_t>(wParam);
    ke.extended = (HIWORD(lParam) & KF_EXTENDED) != 0;
    ev.key = ke;

    return ev;
}

Protocol::InputEvent RawInputCapture::ProcessMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
    Protocol::InputEvent ev;
    ev.timestamp = Timer::NowMs();

    switch (msg) {
    case WM_MOUSEMOVE:
        ev.type = Protocol::InputType::MOUSE_MOVE;
        ev.mouseMove = Protocol::MouseMoveEvent{
            static_cast<int16_t>(LOWORD(lParam)),
            static_cast<int16_t>(HIWORD(lParam))
        };
        break;
    case WM_LBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{0};
        break;
    case WM_LBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{0};
        break;
    case WM_RBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{1};
        break;
    case WM_RBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{1};
        break;
    case WM_MBUTTONDOWN:
        ev.type = Protocol::InputType::MOUSE_BUTTON_DOWN;
        ev.mouseButton = Protocol::MouseButtonEvent{2};
        break;
    case WM_MBUTTONUP:
        ev.type = Protocol::InputType::MOUSE_BUTTON_UP;
        ev.mouseButton = Protocol::MouseButtonEvent{2};
        break;
    case WM_MOUSEWHEEL:
        ev.type = Protocol::InputType::MOUSE_WHEEL;
        ev.mouseWheel = Protocol::MouseWheelEvent{
            static_cast<int16_t>(GET_WHEEL_DELTA_WPARAM(wParam))
        };
        break;
    default:
        break;
    }

    return ev;
}

void RawInputCapture::GetMouseDelta(HRAWINPUT hRawInput, int& dx, int& dy) {
    dx = 0; dy = 0;

    UINT size = 0;
    GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return;

    std::vector<BYTE> buf(size);
    GetRawInputData(hRawInput, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER));

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf.data());

    if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_RELATIVE)) {
        dx = raw->data.mouse.lLastX;
        dy = raw->data.mouse.lLastY;
    }
}
