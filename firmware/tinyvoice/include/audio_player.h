#pragma once

#include <stdint.h>
#include <stddef.h>
#include "volume.h"

class AudioPlayer {
public:
    bool begin();
    void playBootChime();
    bool play(const uint8_t* wavData, size_t len);
    bool playFile(const char* path);
    bool isPlaying() const;
    void loop();
    float volumeGain() const;

private:
    bool _ready;
    bool _playing;
    VolumeControl _volume;
    void playTone(float hz, int durationMs);
};
