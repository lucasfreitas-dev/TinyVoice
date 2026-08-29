#pragma once

#include <stdint.h>
#include <stddef.h>

class AudioPlayer {
public:
    bool begin();
    bool play(const uint8_t* wavData, size_t len);
    bool isPlaying() const;
    void loop();

private:
    bool _playing;
};
