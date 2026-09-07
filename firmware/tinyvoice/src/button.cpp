#include "button.h"
#include "pins.h"
#include "config.h"
#include <Arduino.h>

#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW 1
#endif

static bool isDown() {
#if BUTTON_ACTIVE_LOW
    return digitalRead(BUTTON_PIN) == LOW;
#else
    return digitalRead(BUTTON_PIN) == HIGH;
#endif
}

void Button::begin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    _pressed = false;
    _held = false;
    _justHeld = false;
    _shortPress = false;
    _release = false;
    _pressStartMs = 0;
    _lastChangeMs = millis();
    _rawDown = isDown();
    _stableDown = _rawDown;
    Serial.printf("button init: gpio=%d raw=%s\n", BUTTON_PIN, _stableDown ? "DOWN" : "UP");
}

void Button::loop() {
    _shortPress = false;
    _release = false;
    _justHeld = false;

    bool reading = isDown();
    unsigned long now = millis();

    // Restart the stability window on every raw edge. The previous logic only
    // enforced a gap between *accepted* edges, so a bounce 50 ms after press
    // counted as a real release and started the upload while the button was held.
    if (reading != _rawDown) {
        _rawDown = reading;
        _lastChangeMs = now;
    }

    if (_rawDown != _stableDown &&
        now - _lastChangeMs >= (unsigned long)DEBOUNCE_MS) {
        _stableDown = _rawDown;

        if (_stableDown) {
            _pressed = true;
            _held = false;
            _pressStartMs = now;
            Serial.println("button: down");
        } else {
            if (_pressed && !_held) {
                _shortPress = true;
                Serial.println("button: short press");
            }
            _pressed = false;
            _held = false;
            _release = true;
            Serial.println("button: up");
        }
    }

    if (_pressed && !_held && _stableDown &&
        now - _pressStartMs >= (unsigned long)HOLD_THRESHOLD_MS) {
        _held = true;
        _justHeld = true;
        Serial.println("button: hold");
    }
}

bool Button::isPressed() const { return _pressed; }
bool Button::isHeld() const { return _held; }
bool Button::wasJustHeld() {
    if (_justHeld) {
        _justHeld = false;
        return true;
    }
    return false;
}
bool Button::wasShortPress() { return _shortPress; }
bool Button::wasRelease() { return _release; }
