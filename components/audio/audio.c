// SPDX-License-Identifier: MIT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "board.h"
#include "audio.h"

static const char *TAG = "audio";

#define PLAY_BUF_BYTES   (32 * 1024)     // ~1 s at 16 kHz mono
#define CHUNK_BYTES      1024
#define PA_IDLE_OFF_MS   1500            // amp off after this much silence (no hiss, saves power)
#define DEFAULT_VOLUME   70

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_expander;
static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_spk;
static StreamBufferHandle_t s_play;
static volatile bool s_flush;
static volatile bool s_writing;
static bool s_pa_on;

i2c_master_bus_handle_t audio_i2c_bus(void) { return s_i2c; }

static void set_pa(bool on)
{
    if (on == s_pa_on) return;
    if (s_expander) {
        esp_io_expander_set_level(s_expander, BOARD_EXP_PA_ENABLE_MASK, on ? 1 : 0);
    }
    s_pa_on = on;
}

static void play_task(void *arg)
{
    uint8_t *chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(chunk);
    TickType_t last_audio = 0;

    for (;;) {
        if (s_flush) {
            xStreamBufferReset(s_play);
            s_flush = false;
        }
        size_t n = xStreamBufferReceive(s_play, chunk, CHUNK_BYTES, pdMS_TO_TICKS(100));
        if (n == 0) {
            s_writing = false;
            if (s_pa_on && xTaskGetTickCount() - last_audio > pdMS_TO_TICKS(PA_IDLE_OFF_MS)) {
                set_pa(false);
            }
            continue;
        }
        s_writing = true;
        if (!s_pa_on) {
            set_pa(true);
            vTaskDelay(pdMS_TO_TICKS(20));            // let the amp settle (avoid a click)
        }
        n &= ~(size_t)1;                               // whole samples only
        esp_codec_dev_write(s_spk, chunk, (int)n);
        last_audio = xTaskGetTickCount();
    }
}

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

static void init_expander(void)
{
    esp_err_t err = esp_io_expander_new_i2c_tca95xx_16bit(s_i2c, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000,
                                                          &s_expander);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IO expander not found (%s); assuming the speaker amp is always on",
                 esp_err_to_name(err));
        s_expander = NULL;
        return;
    }
    esp_io_expander_set_dir(s_expander, BOARD_EXP_PA_ENABLE_MASK, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_expander, BOARD_EXP_PA_ENABLE_MASK, 0);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;                            // silence on underrun
    // TODO(mic): create TX+RX together (full duplex) when the ES7210 input lands.
    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;        // same sample on L and R
    return i2s_channel_init_std_mode(s_tx, &std);
}

static esp_err_t init_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = BOARD_I2S_PORT, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) return ESP_FAIL;

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,                                  // amp is behind the IO expander
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&dev_cfg);
    if (!s_spk) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = BOARD_I2S_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_out_vol(s_spk, DEFAULT_VOLUME);
    return ESP_OK;
}

esp_err_t audio_init(void)
{
    esp_err_t err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return err;
    }
    init_expander();
    if ((err = init_i2s()) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = init_codec()) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 speaker codec not responding");
        return err;
    }

    uint8_t *storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    static StaticStreamBuffer_t sb;
    s_play = storage ? xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &sb) : NULL;
    if (!s_play) return ESP_ERR_NO_MEM;

    xTaskCreatePinnedToCore(play_task, "audio_play", 4096, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "speaker ready (%d Hz)", BOARD_I2S_SAMPLE_RATE);
    return ESP_OK;
}

bool audio_mic_ready(void) { return false; }

size_t audio_read_raw(int16_t *buf, size_t n, int ch, uint32_t t)
{
    // TODO(mic): ES7210 via I2S TDM RX
    (void)buf; (void)n; (void)ch; (void)t;
    return 0;
}

esp_err_t audio_play_16k(const int16_t *pcm, size_t samples)
{
    if (!s_play) return ESP_ERR_INVALID_STATE;
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = samples * sizeof(int16_t);
    while (left) {
        size_t sent = xStreamBufferSend(s_play, p, left, pdMS_TO_TICKS(500));
        if (sent == 0 && s_flush) return ESP_OK;      // flushed while waiting
        p += sent;
        left -= sent;
    }
    return ESP_OK;
}

esp_err_t audio_play(const int16_t *pcm, size_t samples)
{
    // TODO(gemini): 24 kHz -> 16 kHz (3:2) resampling
    (void)pcm; (void)samples;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_flush_playback(void)
{
    s_flush = true;
}

bool audio_is_playing(void)
{
    return s_play && (s_writing || xStreamBufferBytesAvailable(s_play) > 0);
}

void audio_set_volume(int volume)
{
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, volume < 0 ? 0 : volume > 100 ? 100 : volume);
}
