#include "volume.h"
#include "pins.h"
#include "config.h"
#include <Arduino.h>
#include <math.h>

#ifndef VOLUME_POT_ENABLED
#define VOLUME_POT_ENABLED 1
#endif

#ifndef VOLUME_MIN_GAIN
#define VOLUME_MIN_GAIN 0.18f
#endif

#ifndef VOLUME_MAX_GAIN
#define VOLUME_MAX_GAIN 1.00f
#endif

static const int ADC_MAX = 4095;
static const int FLOATING_SPREAD = 400;
static const unsigned long VOLUME_POLL_INTERVAL_MS = 25;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float gainFromAdc(int raw) {
    float t = (float)raw / (float)ADC_MAX;
    t = clampf(t, 0.0f, 1.0f);
    // Square the travel so a cheap linear (B-taper) pot feels closer to audio taper.
    float shaped = t * t;
    return VOLUME_MIN_GAIN + (VOLUME_MAX_GAIN - VOLUME_MIN_GAIN) * shaped;
}

int VolumeControl::readAveraged() const {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(VOLUME_POT_PIN);
    }
    return (int)(sum / 8);
}

void VolumeControl::begin() {
    _ready = false;
    _present = false;
    _gain = VOLUME_MAX_GAIN;
    _lastReadMs = 0;
    _lastLoggedPct = -1;

#if !VOLUME_POT_ENABLED
    Serial.println("volume: pot disabled, using max gain");
    _ready = true;
    return;
#endif

    analogReadResolution(12);
    analogSetPinAttenuation(VOLUME_POT_PIN, ADC_11db);
    pinMode(VOLUME_POT_PIN, INPUT);

    int lo = ADC_MAX;
    int hi = 0;
    for (int i = 0; i < 24; i++) {
        int v = analogRead(VOLUME_POT_PIN);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        delay(2);
    }

    // GPIO 34 has no internal pull. A disconnected pin chatters; a wired pot is stable.
    if ((hi - lo) > FLOATING_SPREAD) {
        _present = false;
        _gain = VOLUME_MAX_GAIN;
        Serial.printf("volume: no pot on gpio %d (adc spread %d), using max gain\n",
                      VOLUME_POT_PIN, hi - lo);
    } else {
        _present = true;
        refresh();
        Serial.printf("volume: pot on gpio %d, min gain=%.2f (never mute)\n",
                      VOLUME_POT_PIN, VOLUME_MIN_GAIN);
    }

    _ready = true;
}

void VolumeControl::refresh() {
    int raw = readAveraged();
    _gain = clampf(gainFromAdc(raw), VOLUME_MIN_GAIN, VOLUME_MAX_GAIN);

    int pct = (int)lroundf(_gain * 100.0f);
    if (_lastLoggedPct < 0 || abs(pct - _lastLoggedPct) >= 5) {
        _lastLoggedPct = pct;
        Serial.printf("volume: %d%% (adc=%d)\n", pct, raw);
    }
}

void VolumeControl::poll() {
    if (!_ready || !_present) {
        return;
    }
    unsigned long now = millis();
    if (_lastReadMs != 0 && (now - _lastReadMs) < VOLUME_POLL_INTERVAL_MS) {
        return;
    }
    _lastReadMs = now;
    refresh();
}

float VolumeControl::gain() const { return _gain; }

bool VolumeControl::potPresent() const { return _present; }
