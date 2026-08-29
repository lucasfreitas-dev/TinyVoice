#include "audio_recorder.h"
#include "pins.h"
#include "config.h"
#include "storage_lock.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

static const int SAMPLE_RATE = 16000;
static const size_t WAV_HEADER_SIZE = 44;
static const size_t BYTES_PER_SECOND = SAMPLE_RATE * sizeof(int16_t);
static const i2s_port_t I2S_MIC = I2S_NUM_0;
static const char* REC_DIR = "/rec";
static const int INMP441_SHIFT = 14;
static const size_t I2S_READ_SAMPLES = 256;
static const size_t STAGING_BYTES = 16384;
static const size_t FS_WRITE_MARGIN = 8192;
static const size_t FS_RESERVE_BYTES = 65536;
static const size_t RING_SAMPLE_COUNT = 24576;
static const size_t FLUSH_SLOTS = 2;
static int16_t s_ringBuffer[RING_SAMPLE_COUNT];

struct FlushSlot {
    uint8_t data[STAGING_BYTES];
    size_t len;
    int chunkIndex;
};

static FlushSlot* s_flushSlots = nullptr;
static volatile bool s_slotFree[FLUSH_SLOTS] = {true, true};
static QueueHandle_t s_flushQueue = nullptr;
static TaskHandle_t s_flushTask = nullptr;

static void chunkPath(char* path, size_t len, int index) {
    snprintf(path, len, "%s/c%04d.pcm", REC_DIR, index);
}

static bool writeChunkToDisk(const uint8_t* data, size_t len, int chunkIndex) {
    if (len == 0) {
        return true;
    }

    if (storageFreeBytes() < len + FS_WRITE_MARGIN) {
        Serial.printf("audio: flash full (need %u, free %u)\n",
                      (unsigned)(len + FS_WRITE_MARGIN),
                      (unsigned)storageFreeBytes());
        return false;
    }

    if (!LittleFS.exists(REC_DIR) && !LittleFS.mkdir(REC_DIR)) {
        Serial.println("audio: failed to create /rec");
        return false;
    }

    if (!storageLock()) {
        Serial.println("audio: storage lock timeout on chunk write");
        return false;
    }

    char path[24];
    chunkPath(path, sizeof(path), chunkIndex);
    File file = LittleFS.open(path, FILE_WRITE);
    if (!file) {
        storageUnlock();
        Serial.println("audio: failed to open pcm chunk");
        return false;
    }

    size_t written = file.write(data, len);
    file.close();
    storageUnlock();

    if (written != len) {
        Serial.println("audio: chunk write failed");
        return false;
    }
    return true;
}

static void flushWorkerEntry(void* arg) {
    (void)arg;
    uint8_t slotIdx = 0;
    for (;;) {
        if (xQueueReceive(s_flushQueue, &slotIdx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        FlushSlot& slot = s_flushSlots[slotIdx];
        if (!writeChunkToDisk(slot.data, slot.len, slot.chunkIndex)) {
            Serial.println("audio: background chunk write failed");
        }
        s_slotFree[slotIdx] = true;
    }
}

static bool acquireFlushSlot(int& outSlot, unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        for (size_t i = 0; i < FLUSH_SLOTS; i++) {
            if (s_slotFree[i]) {
                s_slotFree[i] = false;
                outSlot = (int)i;
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void startFlushWorker() {
    if (s_flushSlots == nullptr) {
        s_flushSlots = (FlushSlot*)heap_caps_malloc(sizeof(FlushSlot) * FLUSH_SLOTS, MALLOC_CAP_8BIT);
        if (!s_flushSlots) {
            Serial.println("audio: flush buffer alloc failed");
            return;
        }
        for (size_t i = 0; i < FLUSH_SLOTS; i++) {
            s_slotFree[i] = true;
        }
    }
    if (s_flushQueue == nullptr) {
        s_flushQueue = xQueueCreate(4, sizeof(uint8_t));
    }
    if (s_flushTask == nullptr && s_flushQueue != nullptr) {
        xTaskCreatePinnedToCore(
            flushWorkerEntry,
            "audio_flush",
            4096,
            nullptr,
            4,
            &s_flushTask,
            0
        );
    }
}

static AudioPins pins;
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

bool AudioRecorder::ensureRecordDir() {
    if (!LittleFS.exists(REC_DIR) && !LittleFS.mkdir(REC_DIR)) {
        Serial.println("audio: failed to create /rec");
        return false;
    }
    return true;
}

void AudioRecorder::cleanupRecording() {
    waitForPendingFlushes();

    if (!storageLock()) {
        return;
    }

    File root = LittleFS.open(REC_DIR);
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                char path[32];
                snprintf(path, sizeof(path), "%s/%s", REC_DIR, entry.name());
                entry.close();
                LittleFS.remove(path);
            } else {
                entry.close();
            }
            entry = root.openNextFile();
        }
        root.close();
    }

    storageUnlock();
    _chunkCount = 0;
    _nextChunkIndex = 0;
    _pcmTotal = 0;
    _writePos = 0;
}

int AudioRecorder::chunkCount() const {
    return _chunkCount;
}

bool AudioRecorder::hasPendingRecording() const {
    return _chunkCount > 0 && _pcmTotal > 0;
}

size_t AudioRecorder::recordedPcmBytes() const {
    return _pcmTotal;
}

void AudioRecorder::captureTaskEntry(void* arg) {
    static_cast<AudioRecorder*>(arg)->captureTaskLoop();
}

void AudioRecorder::captureTaskLoop() {
    int32_t raw[I2S_READ_SAMPLES];
    int16_t samples[I2S_READ_SAMPLES];

    for (;;) {
        if (!_recording) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(50));
        if (err != ESP_OK || bytesRead == 0) {
            continue;
        }

        size_t rawCount = bytesRead / sizeof(int32_t);
        for (size_t i = 0; i < rawCount; i++) {
            int32_t s = raw[i] >> INMP441_SHIFT;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
        }
        pushRing(samples, rawCount);
    }
}

void AudioRecorder::pushRing(const int16_t* samples, size_t count) {
    if (!_ring || _ringCapacity == 0) {
        return;
    }
    portENTER_CRITICAL(&ringMux);
    for (size_t i = 0; i < count; i++) {
        if (_ringCount >= _ringCapacity) {
            _ringRead = (_ringRead + 1) % _ringCapacity;
            _ringCount--;
            _droppedSamples++;
        }
        _ring[_ringWrite] = samples[i];
        _ringWrite = (_ringWrite + 1) % _ringCapacity;
        _ringCount++;
    }
    portEXIT_CRITICAL(&ringMux);
}

size_t AudioRecorder::popRing(int16_t* samples, size_t maxCount) {
    size_t popped = 0;

    portENTER_CRITICAL(&ringMux);
    while (popped < maxCount && _ringCount > 0) {
        samples[popped++] = _ring[_ringRead];
        _ringRead = (_ringRead + 1) % _ringCapacity;
        _ringCount--;
    }
    portEXIT_CRITICAL(&ringMux);

    return popped;
}

void AudioRecorder::initRing() {
    _ring = s_ringBuffer;
    _ringCapacity = RING_SAMPLE_COUNT;
}

bool AudioRecorder::allocateStaging() {
    if (_staging) {
        return true;
    }
    _stagingSize = STAGING_BYTES;
    _staging = (uint8_t*)heap_caps_malloc(_stagingSize, MALLOC_CAP_8BIT);
    if (!_staging) {
        Serial.println("audio: staging alloc failed");
    }
    return _staging != nullptr;
}

bool AudioRecorder::begin() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 16,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = pins.micSck,
        .ws_io_num = pins.micWs,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = pins.micSd,
    };

    if (i2s_driver_install(I2S_MIC, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_MIC, &pin_cfg) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_MIC);

    _recording = false;
    _diskFull = false;
    _maxPcmBytes = BYTES_PER_SECOND * MAX_RECORDING_SECONDS;
    _staging = nullptr;
    _stagingSize = 0;
    _writePos = 0;
    _pcmTotal = 0;
    _nextChunkIndex = 0;
    _chunkCount = 0;
    _ringWrite = 0;
    _ringRead = 0;
    _ringCount = 0;
    _droppedSamples = 0;
    _captureTask = nullptr;

    initRing();

    if (!allocateStaging()) {
        return false;
    }

    if (_captureTask == nullptr) {
        xTaskCreatePinnedToCore(
            captureTaskEntry,
            "audio_cap",
            4096,
            this,
            6,
            &_captureTask,
            1
        );
    }

    startFlushWorker();

    Serial.printf("audio: recorder ready (max %u s, staging %u bytes, ring %u samples)\n",
                  (unsigned)MAX_RECORDING_SECONDS,
                  (unsigned)_stagingSize,
                  (unsigned)_ringCapacity);
    return true;
}

bool AudioRecorder::writeChunkSync() {
    if (_writePos == 0 || _diskFull) {
        return true;
    }

    if (!writeChunkToDisk(_staging, _writePos, _nextChunkIndex)) {
        _diskFull = true;
        _recording = false;
        return false;
    }

    _pcmTotal += _writePos;
    _writePos = 0;
    _nextChunkIndex++;
    _chunkCount++;
    return true;
}

bool AudioRecorder::queueChunkFlush() {
    if (_writePos == 0 || _diskFull) {
        return true;
    }
    if (!s_flushSlots) {
        return writeChunkSync();
    }

    int slot = 0;
    if (!acquireFlushSlot(slot, 20)) {
        return writeChunkSync();
    }

    memcpy(s_flushSlots[slot].data, _staging, _writePos);
    s_flushSlots[slot].len = _writePos;
    s_flushSlots[slot].chunkIndex = _nextChunkIndex;

    _pcmTotal += _writePos;
    _writePos = 0;
    _nextChunkIndex++;
    _chunkCount++;

    uint8_t slotIdx = (uint8_t)slot;
    if (xQueueSend(s_flushQueue, &slotIdx, 0) != pdTRUE) {
        s_slotFree[slot] = true;
        return writeChunkSync();
    }
    return true;
}

void AudioRecorder::waitForPendingFlushes() {
    for (int i = 0; i < 400; i++) {
        bool pending = false;
        if (s_flushQueue && uxQueueMessagesWaiting(s_flushQueue) > 0) {
            pending = true;
        }
        for (size_t j = 0; j < FLUSH_SLOTS; j++) {
            if (!s_slotFree[j]) {
                pending = true;
            }
        }
        if (!pending) {
            return;
        }
        delay(10);
    }
    Serial.println("audio: flush wait timeout");
}

bool AudioRecorder::start() {
    if (_recording) {
        Serial.println("audio: already recording");
        return false;
    }
    if (!allocateStaging()) {
        return false;
    }

    storagePruneForRecording();
    cleanupRecording();

    size_t freeBytes = storageFreeBytes();
    if (freeBytes < BYTES_PER_SECOND + FS_RESERVE_BYTES) {
        Serial.printf("audio: not enough flash (free %u)\n", (unsigned)freeBytes);
        return false;
    }

    _maxPcmBytes = BYTES_PER_SECOND * MAX_RECORDING_SECONDS;
    size_t maxByFlash = freeBytes - FS_RESERVE_BYTES;
    if (maxByFlash < _maxPcmBytes) {
        _maxPcmBytes = (maxByFlash / BYTES_PER_SECOND) * BYTES_PER_SECOND;
        Serial.printf("audio: max recording capped to %u s by flash\n",
                      (unsigned)(_maxPcmBytes / BYTES_PER_SECOND));
    }

    portENTER_CRITICAL(&ringMux);
    _ringWrite = 0;
    _ringRead = 0;
    _ringCount = 0;
    _droppedSamples = 0;
    portEXIT_CRITICAL(&ringMux);

    i2s_zero_dma_buffer(I2S_MIC);
    _writePos = 0;
    _pcmTotal = 0;
    _nextChunkIndex = 0;
    _chunkCount = 0;
    _diskFull = false;
    _startMs = millis();
    _recording = true;
    return true;
}

void AudioRecorder::writeSamples(const int16_t* samples, size_t count) {
    if (_diskFull) {
        return;
    }

    size_t offset = 0;
    while (offset < count && !_diskFull) {
        if (_stagingSize == 0) {
            return;
        }

        size_t spaceSamples = (_stagingSize - _writePos) / sizeof(int16_t);
        if (spaceSamples == 0) {
            if (!queueChunkFlush()) {
                return;
            }
            spaceSamples = _stagingSize / sizeof(int16_t);
        }

        size_t remainingPcm = _maxPcmBytes - (_pcmTotal + _writePos);
        if (remainingPcm == 0) {
            return;
        }

        size_t chunk = count - offset;
        if (chunk > spaceSamples) {
            chunk = spaceSamples;
        }
        size_t chunkBytes = chunk * sizeof(int16_t);
        if (chunkBytes > remainingPcm) {
            chunkBytes = remainingPcm;
            chunk = chunkBytes / sizeof(int16_t);
        }

        memcpy(_staging + _writePos, samples + offset, chunkBytes);
        _writePos += chunkBytes;
        offset += chunk;

        if (_writePos >= _stagingSize) {
            queueChunkFlush();
        }
    }
}

size_t AudioRecorder::buildWavHeader(uint8_t* header, size_t dataSize) const {
    uint32_t chunkSize = 36 + dataSize;
    uint32_t byteRate = SAMPLE_RATE * 2;
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16;
    header[20] = 1;
    header[22] = 1;
    header[24] = SAMPLE_RATE & 0xFF;
    header[25] = (SAMPLE_RATE >> 8) & 0xFF;
    header[26] = (SAMPLE_RATE >> 16) & 0xFF;
    header[27] = (SAMPLE_RATE >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = 2;
    header[33] = 0;
    header[34] = 16;
    header[35] = 0;
    memcpy(header + 36, "data", 4);
    header[4] = chunkSize & 0xFF;
    header[5] = (chunkSize >> 8) & 0xFF;
    header[6] = (chunkSize >> 16) & 0xFF;
    header[7] = (chunkSize >> 24) & 0xFF;
    header[40] = dataSize & 0xFF;
    header[41] = (dataSize >> 8) & 0xFF;
    header[42] = (dataSize >> 16) & 0xFF;
    header[43] = (dataSize >> 24) & 0xFF;
    return 44;
}

size_t AudioRecorder::stop(size_t* outFileLen) {
    _recording = false;
    for (int i = 0; i < 100; i++) {
        loop();
        if (_ringCount == 0) {
            break;
        }
        delay(5);
    }
    writeChunkSync();
    waitForPendingFlushes();

    if (_pcmTotal == 0) {
        cleanupRecording();
        if (outFileLen) {
            *outFileLen = 0;
        }
        return 0;
    }

    if (outFileLen) {
        *outFileLen = WAV_HEADER_SIZE + _pcmTotal;
    }
    Serial.printf("recording stopped: %u pcm bytes (~%.1f s, %d chunks)",
                  (unsigned)_pcmTotal, _pcmTotal / (float)BYTES_PER_SECOND, _chunkCount);
    if (_droppedSamples > 0) {
        Serial.printf(", dropped %u samples", (unsigned)_droppedSamples);
    }
    Serial.println();
    return _pcmTotal;
}

bool AudioRecorder::isRecording() const { return _recording; }

bool AudioRecorder::maxDurationReached() const {
    return _recording && (_pcmTotal + _writePos >= _maxPcmBytes);
}

bool AudioRecorder::diskFull() const {
    return _diskFull;
}

void AudioRecorder::loop() {
    if (_ringCount == 0) {
        return;
    }

    int16_t chunk[I2S_READ_SAMPLES];
    size_t popped;
    while ((popped = popRing(chunk, I2S_READ_SAMPLES)) > 0) {
        writeSamples(chunk, popped);
    }
}
