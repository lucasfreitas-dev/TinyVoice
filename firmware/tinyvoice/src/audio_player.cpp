#include "audio_player.h"
#include "pins.h"
#include <driver/i2s.h>

static const int SAMPLE_RATE = 16000;
static const i2s_port_t I2S_SPK = I2S_NUM_1;

static AudioPins pins;

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
    _playing = false;
    return true;
}

bool AudioPlayer::play(const uint8_t* wavData, size_t len) {
    if (len <= 44) return false;

    const uint8_t* pcm = wavData + 44;
    size_t pcmLen = len - 44;
    size_t written = 0;

    _playing = true;
    while (written < pcmLen) {
        size_t chunk = pcmLen - written;
        if (chunk > 512) chunk = 512;
        size_t bytesWritten = 0;
        i2s_write(I2S_SPK, pcm + written, chunk, &bytesWritten, portMAX_DELAY);
        written += bytesWritten;
    }
    _playing = false;
    return true;
}

bool AudioPlayer::isPlaying() const { return _playing; }

void AudioPlayer::loop() {}
