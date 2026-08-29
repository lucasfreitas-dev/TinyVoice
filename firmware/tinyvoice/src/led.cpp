#include "led.h"
#include "pins.h"

void Led::begin() {
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, LOW);
    _lastBlinkMs = 0;
    _blinkOn = false;
}

void Led::update(DeviceState state, bool hasPendingMessage) {
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, LOW);

    switch (state) {
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
            digitalWrite(LED_BLUE_PIN, HIGH);
            break;
        case DeviceState::UPLOADING:
            // blink handled in loop()
            break;
        case DeviceState::PLAYING:
            digitalWrite(LED_GREEN_PIN, HIGH);
            break;
        case DeviceState::ERROR:
            digitalWrite(LED_RED_PIN, HIGH);
            break;
        default:
            if (hasPendingMessage) {
                digitalWrite(LED_GREEN_PIN, HIGH);
            }
            break;
    }
}

void Led::loop() {
    unsigned long now = millis();
    if (now - _lastBlinkMs > 400) {
        _lastBlinkMs = now;
        _blinkOn = !_blinkOn;
    }
}
