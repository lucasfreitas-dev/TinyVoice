#pragma once

#include <stdint.h>
#include <stddef.h>

class AudioRecorder {
public:
    bool begin();
    bool start();
    size_t stop(uint8_t** outWav, size_t* outLen);
    bool isRecording() const;
    void loop();

private:
    bool _recording;
    unsigned long _startMs;
    uint8_t* _buffer;
    size_t _bufferSize;
    size_t _writePos;

    bool allocateBuffer();
    void freeBuffer();
    void writeSamples(const int16_t* samples, size_t count);
    size_t buildWavHeader(uint8_t* header, size_t dataSize);
};
