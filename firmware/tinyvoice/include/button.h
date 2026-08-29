#pragma once

class Button {
public:
    void begin();
    void loop();

    bool isPressed() const;
    bool isHeld() const;
    bool wasShortPress();
    bool wasRelease();

private:
    bool _pressed;
    bool _held;
    bool _shortPress;
    bool _release;
    unsigned long _pressStartMs;
    unsigned long _lastChangeMs;
    bool _lastReading;
};
