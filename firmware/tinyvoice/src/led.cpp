#include "led.h"
#include "pins.h"
#include <Arduino.h>

static const unsigned long WIFI_PHASE_MS[] = {150, 150, 150, 700};

void Led::begin() {
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, LOW);
    _lastBlinkMs = 0;
    _wifiPatternMs = 0;
    _wifiPhase = 0;
    _blinkOn = false;
    _wifiConnected = true;
    _currentState = DeviceState::BOOT;
    _hasPending = false;
    _pressedHint = false;
}

void Led::update(DeviceState state, bool hasPendingMessage) {
    _currentState = state;
    _hasPending = hasPendingMessage;
    applyOutputs();
}

void Led::setPressedHint(bool pressed) {
    _pressedHint = pressed;
    applyOutputs();
}

void Led::setWiFiConnected(bool connected) {
    if (_wifiConnected == connected) {
        return;
    }
    _wifiConnected = connected;
    _wifiPhase = 0;
    _wifiPatternMs = millis();
    applyOutputs();
}

bool Led::wifiPatternActive() const {
    if (_wifiConnected) {
        return false;
    }
    switch (_currentState) {
        case DeviceState::BOOT:
        case DeviceState::CONNECTING_WIFI:
        case DeviceState::RECORDING:
        case DeviceState::UPLOADING:
        case DeviceState::PLAYING:
        case DeviceState::DOWNLOADING:
            return false;
        default:
            return true;
    }
}

void Led::applyOutputs() {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, LOW);

    // Wi-Fi down: double-blink on green (visible on arcade button LED)
    if (wifiPatternActive()) {
        if (_wifiPhase == 0 || _wifiPhase == 2) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            digitalWrite(LED_RED_PIN, HIGH);
        }
        return;
    }

    switch (_currentState) {
        case DeviceState::BOOT:
            digitalWrite(LED_RED_PIN, HIGH);
            digitalWrite(LED_GREEN_PIN, HIGH);
            digitalWrite(LED_BLUE_PIN, HIGH);
            break;
        case DeviceState::CONNECTING_WIFI:
            digitalWrite(LED_RED_PIN, HIGH);
            digitalWrite(LED_GREEN_PIN, HIGH);
            break;
        case DeviceState::RECORDING:
            digitalWrite(LED_GREEN_PIN, HIGH);
            digitalWrite(LED_BLUE_PIN, HIGH);
            break;
        case DeviceState::UPLOADING:
            if (_blinkOn) {
                digitalWrite(LED_GREEN_PIN, HIGH);
            }
            break;
        case DeviceState::PLAYING:
            digitalWrite(LED_GREEN_PIN, HIGH);
            break;
        case DeviceState::ERROR:
            digitalWrite(LED_RED_PIN, HIGH);
            break;
        default:
            if (_hasPending) {
                digitalWrite(LED_GREEN_PIN, HIGH);
            } else if (_pressedHint) {
                digitalWrite(LED_GREEN_PIN, HIGH);
            }
            break;
    }
}

void Led::loop() {
    unsigned long now = millis();

    if (wifiPatternActive()) {
        if (now - _wifiPatternMs >= WIFI_PHASE_MS[_wifiPhase]) {
            _wifiPatternMs = now;
            _wifiPhase = (_wifiPhase + 1) % 4;
            applyOutputs();
        }
        return;
    }

    if (now - _lastBlinkMs > 400) {
        _lastBlinkMs = now;
        _blinkOn = !_blinkOn;
        if (_currentState == DeviceState::UPLOADING) {
            applyOutputs();
        }
    }
}
