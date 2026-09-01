#pragma once

// Analog pot on ADC1 (GPIO 34) → software gain applied to I2S PCM.
// The MAX98357A is Class D: do not put a pot between the amp and the speaker.

class VolumeControl {
public:
    void begin();
    void poll();
    float gain() const;
    bool potPresent() const;

private:
    void refresh();
    int readAveraged() const;

    bool _ready = false;
    bool _present = false;
    float _gain = 1.0f;
    unsigned long _lastReadMs = 0;
    int _lastLoggedPct = -1;
};
