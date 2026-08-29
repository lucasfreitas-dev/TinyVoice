#pragma once

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioRecorder {
public:
    bool begin();
    bool start();
    size_t stop(size_t* outFileLen);
    bool isRecording() const;
    bool maxDurationReached() const;
    bool diskFull() const;
    void loop();
    int chunkCount() const;
    size_t buildWavHeader(uint8_t* header, size_t dataSize) const;
    void cleanupRecording();
    bool hasPendingRecording() const;
    size_t recordedPcmBytes() const;
    void waitForPendingFlushes();

private:
    volatile bool _recording;
    volatile bool _diskFull;
    unsigned long _startMs;
    size_t _maxPcmBytes;
    uint8_t* _staging;
    size_t _stagingSize;
    size_t _writePos;
    size_t _pcmTotal;
    int _nextChunkIndex;
    int _chunkCount;

    int16_t* _ring;
    size_t _ringCapacity;
    volatile size_t _ringWrite;
    volatile size_t _ringRead;
    volatile size_t _ringCount;
    volatile uint32_t _droppedSamples;
    TaskHandle_t _captureTask;

    bool allocateStaging();
    void initRing();
    bool ensureRecordDir();
    bool writeChunkSync();
    bool queueChunkFlush();
    void writeSamples(const int16_t* samples, size_t count);
    void pushRing(const int16_t* samples, size_t count);
    size_t popRing(int16_t* samples, size_t maxCount);
    static void captureTaskEntry(void* arg);
    void captureTaskLoop();
};
