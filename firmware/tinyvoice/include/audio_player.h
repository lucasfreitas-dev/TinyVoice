#pragma once

#include <stdint.h>
#include <stddef.h>

class AudioPlayer {
public:
    bool begin();
    void playBootChime();
    bool play(const uint8_t* wavData, size_t len);
    bool playFile(const char* path);
    bool isPlaying() const;
    void loop();

private:
    bool _ready;
    bool _playing;
    void playTone(float hz, int durationMs);
};
