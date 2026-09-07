#pragma once

#include <stdint.h>
#include "state_machine.h"

class Led {
public:
    void begin();
    void update(DeviceState state, bool hasPendingMessage);
    void setPressedHint(bool pressed);
    void setWiFiConnected(bool connected);
    void setTrimHint(bool trimmed);
    void loop();

private:
    unsigned long _lastBlinkMs;
    unsigned long _wifiPatternMs;
    uint8_t _wifiPhase;
    uint8_t _pulsePhase;
    bool _blinkOn;
    bool _wifiConnected;
    DeviceState _currentState;
    bool _hasPending;
    bool _pressedHint;
    bool _trimHint;

    bool wifiPatternActive() const;
    bool needsBreathe() const;
    unsigned pulseStepMs() const;
    uint8_t breatheDuty(unsigned long now) const;
    void writeGreen(uint8_t duty);
    void writeRgb(bool red, bool blue);
    void applyOutputs();
};
