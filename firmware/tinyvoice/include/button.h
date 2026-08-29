#pragma once

class Button {
public:
    void begin();
    void loop();

    bool isPressed() const;
    bool isHeld() const;
    bool wasJustHeld();
    bool wasShortPress();
    bool wasRelease();

private:
    bool _pressed;
    bool _held;
    bool _justHeld;
    bool _shortPress;
    bool _release;
    bool _stableDown;
    unsigned long _pressStartMs;
    unsigned long _lastChangeMs;
};
