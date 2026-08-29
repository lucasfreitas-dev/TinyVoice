#pragma once

#include "state_machine.h"

class Led {
public:
    void begin();
    void update(DeviceState state, bool hasPendingMessage);
    void loop();

private:
    unsigned long _lastBlinkMs;
    bool _blinkOn;
};
