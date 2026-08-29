#pragma once

#include <stdint.h>
#include "state_machine.h"

class Led {
public:
    void begin();
    void update(DeviceState state, bool hasPendingMessage);
    void setPressedHint(bool pressed);
    void setWiFiConnected(bool connected);
    void loop();

private:
    unsigned long _lastBlinkMs;
    unsigned long _wifiPatternMs;
    uint8_t _wifiPhase;
    bool _blinkOn;
    bool _wifiConnected;
    DeviceState _currentState;
    bool _hasPending;
    bool _pressedHint;

    bool wifiPatternActive() const;
    void applyOutputs();
};
