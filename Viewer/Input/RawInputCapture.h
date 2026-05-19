#pragma once
#include "Common/Protocol/ControlMessage.h"
#include <windows.h>

class RawInputCapture {
public:
    RawInputCapture() = default;

    static Protocol::InputEvent ProcessRawInput(HRAWINPUT hRawInput);
    static Protocol::InputEvent ProcessKeyEvent(UINT msg, WPARAM wParam, LPARAM lParam);
    static Protocol::InputEvent ProcessMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam);

    static void GetMouseDelta(HRAWINPUT hRawInput, int& dx, int& dy);
};
