#include "button.h"
#include "pins.h"
#include "config.h"

void Button::begin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    _pressed = false;
    _held = false;
    _shortPress = false;
    _release = false;
    _lastReading = digitalRead(BUTTON_PIN) == LOW;
    _pressStartMs = 0;
    _lastChangeMs = 0;
}

void Button::loop() {
    _shortPress = false;
    _release = false;

    bool reading = digitalRead(BUTTON_PIN) == LOW;
    unsigned long now = millis();

    if (reading != _lastReading && now - _lastChangeMs > DEBOUNCE_MS) {
        _lastChangeMs = now;
        _lastReading = reading;

        if (reading) {
            _pressed = true;
            _pressStartMs = now;
        } else {
            _pressed = false;
            _held = false;
            _release = true;
            if (now - _pressStartMs < HOLD_THRESHOLD_MS) {
                _shortPress = true;
            }
        }
    }

    if (_pressed && !_held && now - _pressStartMs >= HOLD_THRESHOLD_MS) {
        _held = true;
    }
}

bool Button::isPressed() const { return _pressed; }
bool Button::isHeld() const { return _held; }
bool Button::wasShortPress() { return _shortPress; }
bool Button::wasRelease() { return _release; }
