#include "audio_player.h"
#include "pins.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

static void applyPcmGain(uint8_t* buf, size_t nbytes, float gain) {
    if (buf == nullptr || nbytes < 2) {
        return;
    }
    if (gain >= 0.999f) {
        return;
    }
    if (gain < 0.0f) {
        gain = 0.0f;
    }
    int32_t q15 = (int32_t)(gain * 32768.0f + 0.5f);
    if (q15 < 0) {
        q15 = 0;
    }
    if (q15 > 32768) {
        q15 = 32768;
    }
    size_t samples = nbytes / 2;
    for (size_t i = 0; i < samples; i++) {
        int16_t sample = (int16_t)(buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8));
        int32_t scaled = (sample * q15) >> 15;
        if (scaled > 32767) {
            scaled = 32767;
        }
        if (scaled < -32768) {
            scaled = -32768;
        }
        buf[i * 2] = (uint8_t)(scaled & 0xFF);
        buf[i * 2 + 1] = (uint8_t)((scaled >> 8) & 0xFF);
    }
}

static const int SAMPLE_RATE = 16000;
static const i2s_port_t I2S_SPK = I2S_NUM_1;

static AudioPins pins;

static size_t findWavPcmOffset(const uint8_t* wavData, size_t len) {
    if (len < 12 || memcmp(wavData, "RIFF", 4) != 0 || memcmp(wavData + 8, "WAVE", 4) != 0) {
        return 0;
    }

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t* chunkId = wavData + pos;
        uint32_t chunkSize = (uint32_t)chunkId[4]
            | ((uint32_t)chunkId[5] << 8)
            | ((uint32_t)chunkId[6] << 16)
            | ((uint32_t)chunkId[7] << 24);
        pos += 8;
        if (memcmp(chunkId, "data", 4) == 0) {
            return pos;
        }
        if (pos + chunkSize > len) {
            return 0;
        }
        pos += chunkSize;
        if (chunkSize % 2 == 1) {
            pos++;
        }
    }
    return 0;
}

bool AudioPlayer::begin() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pin_cfg = {
        .bck_io_num = pins.speakerBclk,
        .ws_io_num = pins.speakerLrc,
        .data_out_num = pins.speakerDin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    if (i2s_driver_install(I2S_SPK, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_SPK, &pin_cfg) != ESP_OK) return false;
    _ready = false;
    _playing = false;
    _volume.begin();
    _ready = true;
    return true;
}

void AudioPlayer::playTone(float hz, int durationMs) {
    if (!_ready || hz <= 0 || durationMs <= 0) {
        return;
    }

    _volume.poll();
    int16_t buf[256];
    int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    int produced = 0;

    while (produced < totalSamples) {
        int block = totalSamples - produced;
        if (block > (int)(sizeof(buf) / sizeof(buf[0]))) {
            block = sizeof(buf) / sizeof(buf[0]);
        }

        for (int i = 0; i < block; i++) {
            float t = (float)(produced + i) / SAMPLE_RATE;
            float progress = (float)(produced + i) / totalSamples;
            float attack = progress < 0.08f ? progress / 0.08f : 1.0f;
            float release = progress > 0.80f ? (1.0f - progress) / 0.20f : 1.0f;
            float env = attack * release;
            float sample = sinf(2.0f * PI * hz * t) * 7000.0f * env * _volume.gain();
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            buf[i] = (int16_t)sample;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_SPK, buf, block * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
        produced += block;
        _volume.poll();
    }
}

void AudioPlayer::playBootChime() {
    if (!_ready) {
        return;
    }

    _playing = true;
    const float notes[] = {523.25f, 659.25f, 783.99f};
    for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        playTone(notes[i], 130);
        delay(35);
    }
    _playing = false;
    Serial.println("audio: boot chime played");
}

bool AudioPlayer::play(const uint8_t* wavData, size_t len) {
    if (!_ready || len <= 44) return false;

    size_t offset = findWavPcmOffset(wavData, len);
    if (offset == 0) {
        Serial.println("audio: invalid wav header");
        return false;
    }

    const uint8_t* pcm = wavData + offset;
    size_t pcmLen = len - offset;
    size_t written = 0;

    _playing = true;
    uint8_t buf[512];
    while (written < pcmLen) {
        size_t chunk = pcmLen - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        memcpy(buf, pcm + written, chunk);
        _volume.poll();
        applyPcmGain(buf, chunk, _volume.gain());
        size_t bytesWritten = 0;
        i2s_write(I2S_SPK, buf, chunk, &bytesWritten, portMAX_DELAY);
        written += bytesWritten;
        yield();
    }
    _playing = false;
    Serial.printf("audio: played %u pcm bytes\n", (unsigned)pcmLen);
    return true;
}

bool AudioPlayer::playFile(const char* path) {
    if (!_ready) return false;

    File f = LittleFS.open(path, FILE_READ);
    if (!f) {
        Serial.println("audio: open failed");
        return false;
    }

    uint8_t header[256];
    size_t headerLen = f.read(header, sizeof(header));
    size_t offset = findWavPcmOffset(header, headerLen);
    if (offset == 0) {
        Serial.println("audio: invalid wav header");
        f.close();
        return false;
    }

    size_t pcmLen = f.size() - offset;
    if (!f.seek(offset)) {
        Serial.println("audio: seek failed");
        f.close();
        return false;
    }

    uint8_t buf[512];
    size_t written = 0;
    _playing = true;
    while (written < pcmLen) {
        size_t toRead = pcmLen - written;
        if (toRead > sizeof(buf)) {
            toRead = sizeof(buf);
        }
        size_t n = f.read(buf, toRead);
        if (n == 0) {
            break;
        }
        _volume.poll();
        applyPcmGain(buf, n, _volume.gain());
        size_t bytesWritten = 0;
        i2s_write(I2S_SPK, buf, n, &bytesWritten, portMAX_DELAY);
        written += n;
        yield();
    }
    f.close();
    _playing = false;
    Serial.printf("audio: played %u pcm bytes from file\n", (unsigned)written);
    return written > 0;
}

bool AudioPlayer::isPlaying() const { return _playing; }

float AudioPlayer::volumeGain() const { return _volume.gain(); }

void AudioPlayer::loop() {
    _volume.poll();
}
