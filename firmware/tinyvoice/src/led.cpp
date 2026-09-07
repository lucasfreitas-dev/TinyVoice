#include "led.h"
#include "pins.h"
#include <Arduino.h>
#include <math.h>

static const int GREEN_LEDC_CH = 0;
static const int GREEN_LEDC_FREQ = 5000;
static const int GREEN_LEDC_BITS = 8;

static const unsigned long WIFI_PHASE_MS[] = {150, 150, 150, 700};
static const unsigned UPLOAD_ON_MS[] = {100, 300, 600};
static const unsigned DOWNLOAD_ON_MS[] = {600, 300, 100};
static const unsigned PULSE_OFF_MS = 180;
static const unsigned FAST_PULSE_MS = 50;
static const unsigned TRIM_BLINK_MS = 400;
static const unsigned long BREATHE_PERIOD_MS = 1800;
static const uint8_t BREATHE_MIN = 32;
static const uint8_t BREATHE_MAX = 255;

void Led::begin() {
    ledcSetup(GREEN_LEDC_CH, GREEN_LEDC_FREQ, GREEN_LEDC_BITS);
    ledcAttachPin(LED_GREEN_PIN, GREEN_LEDC_CH);
    ledcWrite(GREEN_LEDC_CH, 0);

    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);
    digitalWrite(LED_RED_PIN, LOW);
    digitalWrite(LED_BLUE_PIN, LOW);

    _lastBlinkMs = 0;
    _wifiPatternMs = 0;
    _wifiPhase = 0;
    _pulsePhase = 0;
    _blinkOn = false;
    _wifiConnected = true;
    _currentState = DeviceState::BOOT;
    _hasPending = false;
    _pressedHint = false;
    _trimHint = false;
}

void Led::update(DeviceState state, bool hasPendingMessage) {
    if (state != _currentState) {
        _lastBlinkMs = millis();
        _pulsePhase = 0;
        // Drop off solid recording immediately, then start the pulse cadence.
        if (state == DeviceState::PROCESSING) {
            _blinkOn = false;
        } else {
            _blinkOn = true;
        }
    }
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

void Led::setTrimHint(bool trimmed) {
    if (_trimHint == trimmed) {
        return;
    }
    _trimHint = trimmed;
    _lastBlinkMs = millis();
    _blinkOn = true;
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
        case DeviceState::PROCESSING:
        case DeviceState::UPLOADING:
        case DeviceState::PLAYING:
        case DeviceState::DOWNLOADING:
            return false;
        default:
            return true;
    }
}

bool Led::needsBreathe() const {
    if (wifiPatternActive() || !_hasPending) {
        return false;
    }
    return _currentState == DeviceState::IDLE ||
           _currentState == DeviceState::CHECKING_MESSAGES;
}

unsigned Led::pulseStepMs() const {
    switch (_currentState) {
        case DeviceState::PROCESSING:
            return FAST_PULSE_MS;
        case DeviceState::UPLOADING:
            return _blinkOn ? UPLOAD_ON_MS[_pulsePhase % 3] : PULSE_OFF_MS;
        case DeviceState::DOWNLOADING:
            return _blinkOn ? DOWNLOAD_ON_MS[_pulsePhase % 3] : PULSE_OFF_MS;
        case DeviceState::PLAYING:
            return _trimHint ? TRIM_BLINK_MS : 0;
        default:
            return 0;
    }
}

uint8_t Led::breatheDuty(unsigned long now) const {
    const float kPi = 3.14159265f;
    float x = (2.0f * kPi * (float)(now % BREATHE_PERIOD_MS)) / (float)BREATHE_PERIOD_MS;
    float s = 0.5f * (1.0f + sinf(x - kPi / 2.0f));
    return (uint8_t)(BREATHE_MIN + (BREATHE_MAX - BREATHE_MIN) * s);
}

void Led::writeGreen(uint8_t duty) {
    ledcWrite(GREEN_LEDC_CH, duty);
}

void Led::writeRgb(bool red, bool blue) {
    digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
    digitalWrite(LED_BLUE_PIN, blue ? HIGH : LOW);
}

void Led::applyOutputs() {
    writeRgb(false, false);
    writeGreen(0);

    if (wifiPatternActive()) {
        if (_wifiPhase == 0 || _wifiPhase == 2) {
            writeGreen(255);
            writeRgb(true, false);
        }
        return;
    }

    switch (_currentState) {
        case DeviceState::BOOT:
            writeGreen(255);
            writeRgb(true, true);
            break;
        case DeviceState::CONNECTING_WIFI:
            writeGreen(255);
            writeRgb(true, false);
            break;
        case DeviceState::RECORDING:
            writeGreen(255);
            writeRgb(false, true);
            break;
        case DeviceState::PROCESSING:
            if (_blinkOn) {
                writeGreen(255);
            }
            break;
        case DeviceState::UPLOADING:
            if (_blinkOn) {
                writeGreen(255);
            }
            break;
        case DeviceState::DOWNLOADING:
            if (_blinkOn) {
                writeGreen(255);
                writeRgb(false, true);
            }
            break;
        case DeviceState::PLAYING:
            if (_trimHint) {
                if (_blinkOn) {
                    writeGreen(255);
                }
            } else {
                writeGreen(255);
            }
            break;
        case DeviceState::ERROR:
            writeRgb(true, false);
            break;
        default:
            if (_hasPending) {
                writeGreen(breatheDuty(millis()));
            } else if (_pressedHint) {
                writeGreen(255);
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

    if (needsBreathe()) {
        applyOutputs();
        return;
    }

    unsigned step = pulseStepMs();
    if (step == 0) {
        return;
    }
    if (now - _lastBlinkMs >= step) {
        _lastBlinkMs = now;
        if (_blinkOn) {
            _pulsePhase++;
        }
        _blinkOn = !_blinkOn;
        applyOutputs();
    }
}
