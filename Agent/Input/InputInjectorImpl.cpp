#include "InputInjectorImpl.h"
#include "Common/Utils/Logger.h"
#include <windows.h>

InputInjectorImpl::InputInjectorImpl() {}
InputInjectorImpl::~InputInjectorImpl() {}

bool InputInjectorImpl::Inject(const Protocol::InputEvent& event) {
    switch (event.type) {
    case Protocol::InputType::MOUSE_MOVE:
        if (event.mouseMove) {
            return InjectMouseMove(event.mouseMove->dx, event.mouseMove->dy);
        }
        break;
    case Protocol::InputType::MOUSE_BUTTON_DOWN:
        if (event.mouseButton) {
            return InjectMouseButton(event.mouseButton->button, true);
        }
        break;
    case Protocol::InputType::MOUSE_BUTTON_UP:
        if (event.mouseButton) {
            return InjectMouseButton(event.mouseButton->button, false);
        }
        break;
    case Protocol::InputType::MOUSE_WHEEL:
        if (event.mouseWheel) {
            return InjectMouseWheel(event.mouseWheel->delta);
        }
        break;
    case Protocol::InputType::KEY_DOWN:
        if (event.key) {
            return InjectKey(event.key->vkCode, event.key->extended, true);
        }
        break;
    case Protocol::InputType::KEY_UP:
        if (event.key) {
            return InjectKey(event.key->vkCode, event.key->extended, false);
        }
        break;
    default:
        break;
    }
    return false;
}

bool InputInjectorImpl::InjectMouseMove(int16_t dx, int16_t dy) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;

    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool InputInjectorImpl::InjectMouseButton(uint8_t button, bool down) {
    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
    case 0: // Left
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1: // Right
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 2: // Middle
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return false;
    }

    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool InputInjectorImpl::InjectMouseWheel(int16_t delta) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta) * WHEEL_DELTA;

    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool InputInjectorImpl::InjectKey(uint16_t vkCode, bool extended, bool down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkCode;
    input.ki.wScan = static_cast<WORD>(MapVirtualKey(vkCode, MAPVK_VK_TO_VSC));

    if (extended) {
        input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    }
    if (!down) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    return SendInput(1, &input, sizeof(INPUT)) == 1;
}
