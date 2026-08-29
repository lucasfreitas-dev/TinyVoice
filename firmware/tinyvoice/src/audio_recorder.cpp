#include "audio_recorder.h"
#include "pins.h"
#include "config.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>

static const int SAMPLE_RATE = 16000;
static const i2s_port_t I2S_MIC = I2S_NUM_0;

static AudioPins pins;

bool AudioRecorder::begin() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
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
    _buffer = nullptr;
    _bufferSize = 0;
    _writePos = 0;
    return true;
}

bool AudioRecorder::allocateBuffer() {
    size_t maxSamples = SAMPLE_RATE * MAX_RECORDING_SECONDS;
    _bufferSize = maxSamples * sizeof(int16_t);
    _buffer = (uint8_t*)heap_caps_malloc(_bufferSize, MALLOC_CAP_8BIT);
    return _buffer != nullptr;
}

void AudioRecorder::freeBuffer() {
    if (_buffer) {
        free(_buffer);
        _buffer = nullptr;
    }
    _bufferSize = 0;
    _writePos = 0;
}

bool AudioRecorder::start() {
    if (_recording) return false;
    if (!allocateBuffer()) return false;
    i2s_zero_dma_buffer(I2S_MIC);
    _writePos = 0;
    _startMs = millis();
    _recording = true;
    return true;
}

void AudioRecorder::writeSamples(const int16_t* samples, size_t count) {
    size_t bytes = count * sizeof(int16_t);
    if (_writePos + bytes > _bufferSize) {
        bytes = _bufferSize - _writePos;
    }
    memcpy(_buffer + _writePos, samples, bytes);
    _writePos += bytes;
}

size_t AudioRecorder::buildWavHeader(uint8_t* header, size_t dataSize) {
    uint32_t chunkSize = 36 + dataSize;
    uint32_t byteRate = SAMPLE_RATE * 2;
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16; // subchunk size
    header[20] = 1;  // PCM
    header[22] = 1;  // mono
    header[24] = SAMPLE_RATE & 0xFF;
    header[25] = (SAMPLE_RATE >> 8) & 0xFF;
    header[26] = (SAMPLE_RATE >> 16) & 0xFF;
    header[27] = (SAMPLE_RATE >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = 2;  // block align
    header[33] = 0;
    header[34] = 16; // bits per sample
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

size_t AudioRecorder::stop(uint8_t** outWav, size_t* outLen) {
    _recording = false;

    if (_writePos == 0) {
        freeBuffer();
        return 0;
    }

    size_t wavLen = 44 + _writePos;
    uint8_t* wav = (uint8_t*)malloc(wavLen);
    if (!wav) {
        freeBuffer();
        return 0;
    }

    buildWavHeader(wav, _writePos);
    memcpy(wav + 44, _buffer, _writePos);
    freeBuffer();

    *outWav = wav;
    *outLen = wavLen;
    return _writePos;
}

bool AudioRecorder::isRecording() const { return _recording; }

void AudioRecorder::loop() {
    if (!_recording) return;

    if (millis() - _startMs >= (unsigned long)MAX_RECORDING_SECONDS * 1000UL) {
        return;
    }

    int16_t samples[256];
    size_t bytesRead = 0;
    if (i2s_read(I2S_MIC, samples, sizeof(samples), &bytesRead, 0) == ESP_OK && bytesRead > 0) {
        writeSamples(samples, bytesRead / sizeof(int16_t));
    }
}
