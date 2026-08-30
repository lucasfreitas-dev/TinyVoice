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
// Capture fills one chunk slot directly and hands it to the flush worker, so there is no
// separate staging buffer and no memcpy per chunk. Do not shrink this: each chunk is its
// own LittleFS file, and at 8 KB the create-per-chunk cost drops write throughput below
// the 32 KB/s capture rate.
static const size_t CHUNK_BYTES = 16384;
static const size_t FS_WRITE_MARGIN = 8192;
// Keep flash headroom for LittleFS metadata and in-place chunk upload (no WAV copy).
static const size_t FS_UPLOAD_SAFE_BYTES = 24576;
// One second of capture headroom: a LittleFS chunk write must never outlast the ring.
static const size_t RING_SAMPLE_COUNT = 16384;
// One slot is the live capture target, the other absorbs the write still in flight.
static const size_t FLUSH_SLOTS = 2;
static int16_t s_ringBuffer[RING_SAMPLE_COUNT];
static AudioRecorder* s_recorderInstance = nullptr;
// Loudest sample of the current take, so a dead or mis-wired mic is visible in the log.
static volatile uint32_t s_peakAmplitude = 0;

static size_t computeMaxPcmBytes() {
    size_t freeBytes = storageFreeBytes();
    if (freeBytes <= FS_UPLOAD_SAFE_BYTES + FS_WRITE_MARGIN) {
        return 0;
    }

    size_t usable = freeBytes - FS_UPLOAD_SAFE_BYTES - FS_WRITE_MARGIN;
    usable = (usable / CHUNK_BYTES) * CHUNK_BYTES;

    size_t cap = (size_t)MAX_RECORDING_SECONDS * BYTES_PER_SECOND;
    if (usable > cap) {
        usable = cap;
    }
    return usable;
}

struct FlushSlot {
    uint8_t data[CHUNK_BYTES];
    size_t len;
    int chunkIndex;
};

// Statically reserved: a heap block this large fragmented the heap for the rest of the
// uptime, leaving mbedTLS without a contiguous span even with plenty of free bytes.
static FlushSlot s_flushSlots[FLUSH_SLOTS];
static volatile bool s_slotFree[FLUSH_SLOTS] = {true, true};
// Slot currently being filled by capture; owned by the recorder, never by the worker.
static int s_activeSlot = -1;
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
            if (s_recorderInstance) {
                s_recorderInstance->markFlashLimit();
            }
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
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void startFlushWorker() {
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

void AudioRecorder::releaseMemoryForNetwork() {
    waitForPendingFlushes();
    if (_recording) {
        return;
    }
    _writePos = 0;
}

bool AudioRecorder::canAssembleUploadWav(size_t pcmBytes) const {
    size_t needed = pcmBytes + 44 + 4096;
    return storageFreeBytes() >= needed;
}

bool AudioRecorder::assembleUploadWav(const char* destPath, const uint8_t* wavHeader,
                                      size_t pcmBytes, int chunkCount) {
    waitForPendingFlushes();

    if (!canAssembleUploadWav(pcmBytes)) {
        Serial.printf("upload: skip assemble (need %u, free %u)\n",
                      (unsigned)(pcmBytes + 44 + 4096),
                      (unsigned)storageFreeBytes());
        return false;
    }

    if (!ensureRecordDir()) {
        return false;
    }

    if (LittleFS.exists(destPath)) {
        LittleFS.remove(destPath);
    }

    if (!storageLock()) {
        Serial.println("upload: assemble lock timeout");
        return false;
    }

    File out = LittleFS.open(destPath, FILE_WRITE);
    if (!out) {
        storageUnlock();
        Serial.println("upload: assemble open failed");
        return false;
    }

    if (out.write(wavHeader, 44) != 44) {
        out.close();
        storageUnlock();
        LittleFS.remove(destPath);
        Serial.println("upload: assemble header write failed");
        return false;
    }

    size_t copied = 0;
    uint8_t buf[1024];
    for (int i = 0; i < chunkCount; i++) {
        char path[24];
        snprintf(path, sizeof(path), "/rec/c%04d.pcm", i);
        File in = LittleFS.open(path, FILE_READ);
        if (!in) {
            Serial.printf("upload: missing chunk %s\n", path);
            out.close();
            storageUnlock();
            LittleFS.remove(destPath);
            return false;
        }

        while (in.available()) {
            size_t n = in.read(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (out.write(buf, n) != n) {
                in.close();
                out.close();
                storageUnlock();
                LittleFS.remove(destPath);
                Serial.println("upload: assemble pcm write failed");
                return false;
            }
            copied += n;
        }
        in.close();
        yield();
    }

    out.close();
    storageUnlock();

    if (copied != pcmBytes) {
        Serial.printf("upload: assemble size mismatch (%u != %u)\n",
                      (unsigned)copied, (unsigned)pcmBytes);
        LittleFS.remove(destPath);
        return false;
    }

    Serial.printf("upload: assembled %u bytes\n", (unsigned)(44 + copied));
    return true;
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
        uint32_t peak = s_peakAmplitude;
        for (size_t i = 0; i < rawCount; i++) {
            int32_t s = raw[i] >> INMP441_SHIFT;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;

            uint32_t magnitude = (uint32_t)(s < 0 ? -s : s);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        s_peakAmplitude = peak;
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

// Takes ownership of a chunk slot and makes it the live capture target.
bool AudioRecorder::allocateStaging() {
    if (s_activeSlot >= 0) {
        _staging = s_flushSlots[s_activeSlot].data;
        _stagingSize = CHUNK_BYTES;
        return true;
    }

    int slot = 0;
    if (!acquireFlushSlot(slot, 1000)) {
        Serial.println("audio: no free chunk slot");
        _staging = nullptr;
        _stagingSize = 0;
        return false;
    }

    s_activeSlot = slot;
    _staging = s_flushSlots[slot].data;
    _stagingSize = CHUNK_BYTES;
    return true;
}

bool AudioRecorder::begin() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        // INMP441 with L/R tied to GND drives the WS-low slot, which the ESP32 legacy I2S
        // RX path exposes as ONLY_RIGHT. ONLY_LEFT samples the idle slot, i.e. silence.
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
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

    Serial.printf("audio: recorder ready (max %u s, %u chunk slots of %u bytes, ring %u samples)\n",
                  (unsigned)MAX_RECORDING_SECONDS,
                  (unsigned)FLUSH_SLOTS,
                  (unsigned)CHUNK_BYTES,
                  (unsigned)_ringCapacity);
    return true;
}

bool AudioRecorder::hasSpaceForChunk(size_t bytes) const {
    return storageFreeBytes() >= bytes + FS_WRITE_MARGIN;
}

void AudioRecorder::markFlashLimit() {
    if (_diskFull) {
        return;
    }
    _diskFull = true;
    _recording = false;
    Serial.println("audio: flash limit reached (safe for upload)");
}

bool AudioRecorder::writeChunkSync() {
    if (_writePos == 0 || _diskFull) {
        return true;
    }

    if (!hasSpaceForChunk(_writePos)) {
        markFlashLimit();
        return false;
    }

    if (!writeChunkToDisk(_staging, _writePos, _nextChunkIndex)) {
        markFlashLimit();
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

    if (!hasSpaceForChunk(_writePos)) {
        markFlashLimit();
        return false;
    }

    if (s_activeSlot < 0 || !s_flushQueue) {
        return writeChunkSync();
    }

    // Capture can only be handed off if there is another slot to continue into; otherwise
    // write this one here, which keeps the ring draining instead of stalling on the worker.
    int nextSlot = 0;
    if (!acquireFlushSlot(nextSlot, 500)) {
        return writeChunkSync();
    }

    int filled = s_activeSlot;
    s_flushSlots[filled].len = _writePos;
    s_flushSlots[filled].chunkIndex = _nextChunkIndex;

    uint8_t slotIdx = (uint8_t)filled;
    if (xQueueSend(s_flushQueue, &slotIdx, 0) != pdTRUE) {
        s_slotFree[nextSlot] = true;
        return writeChunkSync();
    }

    s_activeSlot = nextSlot;
    _staging = s_flushSlots[nextSlot].data;

    _pcmTotal += _writePos;
    _writePos = 0;
    _nextChunkIndex++;
    _chunkCount++;
    return true;
}

void AudioRecorder::waitForPendingFlushes() {
    for (int i = 0; i < 400; i++) {
        bool pending = false;
        if (s_flushQueue && uxQueueMessagesWaiting(s_flushQueue) > 0) {
            pending = true;
        }
        for (size_t j = 0; j < FLUSH_SLOTS; j++) {
            // The live capture slot is permanently held by the recorder, not the worker.
            if ((int)j != s_activeSlot && !s_slotFree[j]) {
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
    startFlushWorker();
    if (!allocateStaging()) {
        return false;
    }

    s_recorderInstance = this;

    storagePruneForRecording();
    cleanupRecording();

    _maxPcmBytes = computeMaxPcmBytes();
    if (_maxPcmBytes < BYTES_PER_SECOND / 2) {
        Serial.printf("audio: not enough flash (free %u)\n", (unsigned)storageFreeBytes());
        return false;
    }

    unsigned maxSec = (unsigned)(_maxPcmBytes / BYTES_PER_SECOND);
    if (maxSec < (unsigned)MAX_RECORDING_SECONDS) {
        Serial.printf("audio: max recording capped to %u s by flash (safe for upload)\n", maxSec);
    }

    portENTER_CRITICAL(&ringMux);
    _ringWrite = 0;
    _ringRead = 0;
    _ringCount = 0;
    _droppedSamples = 0;
    portEXIT_CRITICAL(&ringMux);

    s_peakAmplitude = 0;
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
            _recording = false;
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
    Serial.printf("recording stopped: %u pcm bytes (~%.1f s, %d chunks, peak %u)",
                  (unsigned)_pcmTotal, _pcmTotal / (float)BYTES_PER_SECOND, _chunkCount,
                  (unsigned)s_peakAmplitude);
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

bool AudioRecorder::flashLimitReached() const {
    return _diskFull;
}

unsigned AudioRecorder::maxRecordingSeconds() const {
    return (unsigned)(_maxPcmBytes / BYTES_PER_SECOND);
}

void AudioRecorder::loop() {
    if (_ringCount == 0) {
        return;
    }

    int16_t chunk[I2S_READ_SAMPLES];
    size_t popped;
    // Bounded drain. Capture keeps filling the ring while we are in here, so draining
    // until empty never terminates once flash writes fall behind real time — which hangs
    // the main loop and with it the button. The ring absorbs whatever we leave behind.
    const size_t budget = _ringCapacity / 2;
    size_t drained = 0;

    while (drained < budget && (popped = popRing(chunk, I2S_READ_SAMPLES)) > 0) {
        writeSamples(chunk, popped);
        drained += popped;
    }
}
