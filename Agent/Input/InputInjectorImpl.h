#pragma once
#include "Common/Protocol/ControlMessage.h"

class InputInjectorImpl {
public:
    InputInjectorImpl();
    ~InputInjectorImpl();

    bool Inject(const Protocol::InputEvent& event);

private:
    bool InjectMouseMove(int16_t dx, int16_t dy);
    bool InjectMouseButton(uint8_t button, bool down);
    bool InjectMouseWheel(int16_t delta);
    bool InjectKey(uint16_t vkCode, bool extended, bool down);
};
