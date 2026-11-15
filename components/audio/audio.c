// SPDX-License-Identifier: MIT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "es7210_adc.h"
#include "sdkconfig.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "board.h"
#include "dsp.h"
#include "audio.h"

static const char *TAG = "audio";

#define PLAY_BUF_BYTES   (512 * 1024)    // ~16 s at 16 kHz mono (Gemini sends faster than real time)
#define CHUNK_BYTES      1024
#define PA_IDLE_OFF_MS   1500            // amp off after this much silence (no hiss, saves power)
#define DEFAULT_VOLUME   70

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_expander;
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_spk, s_mic;
static const audio_codec_data_if_t *s_data_if;
static bool s_mic_ok;

#define MIC_CHANNELS   4                 // ES7210 TDM slots; layout = CONFIG_VA_AFE_INPUT_FORMAT
#define MIC_GAIN_DB    30.0f
#define REF_GAIN_DB    0.0f
static float s_level_db[MIC_CHANNELS] = { -96, -96, -96, -96 };
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
    // Both codecs share MCLK/BCLK/WS: one I2S port in full duplex.
    // TX (ES8311 speaker) uses standard Philips framing, RX (ES7210, 4 channels) uses TDM.
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;                            // silence on underrun
    esp_err_t err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) return err;

    const i2s_std_gpio_config_t std_gpio = {
        .mclk = BOARD_I2S_MCLK, .bclk = BOARD_I2S_BCLK, .ws = BOARD_I2S_WS,
        .dout = BOARD_I2S_DOUT, .din = BOARD_I2S_DIN,
    };
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = std_gpio,
    };
    if ((err = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) return err;

    i2s_tdm_config_t tdm = {
        .clk_cfg = {
            .sample_rate_hz = BOARD_I2S_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK, .bclk = BOARD_I2S_BCLK, .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT, .din = BOARD_I2S_DIN,
        },
    };
    return i2s_channel_init_tdm_mode(s_rx, &tdm);
}

static esp_err_t init_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = BOARD_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    s_data_if = data_if;

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

/** ES7210 4-channel ADC: 2 microphones + playback reference for echo cancellation. */
static esp_err_t init_mic(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if || !s_data_if) return ESP_FAIL;

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es_cfg);
    if (!codec_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = s_data_if,
    };
    s_mic = esp_codec_dev_new(&dev_cfg);
    if (!s_mic) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = MIC_CHANNELS,
        .sample_rate = BOARD_I2S_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;

    // Mic channels get gain; the playback reference channel stays at 0 dB.
    const char *fmt = CONFIG_VA_AFE_INPUT_FORMAT;
    for (int ch = 0; ch < MIC_CHANNELS && fmt[ch]; ch++) {
        esp_codec_dev_set_in_channel_gain(s_mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(ch),
                                          fmt[ch] == 'R' ? REF_GAIN_DB : MIC_GAIN_DB);
    }
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
    if (init_mic() == ESP_OK) {
        s_mic_ok = true;
        ESP_LOGI(TAG, "microphones ready (%d channels, layout %s)", MIC_CHANNELS,
                 CONFIG_VA_AFE_INPUT_FORMAT);
    } else {
        ESP_LOGE(TAG, "ES7210 microphone codec not responding");   // speaker still usable
    }

    uint8_t *storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    static StaticStreamBuffer_t sb;
    s_play = storage ? xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &sb) : NULL;
    if (!s_play) return ESP_ERR_NO_MEM;

    xTaskCreatePinnedToCore(play_task, "audio_play", 4096, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "speaker ready (%d Hz)", BOARD_I2S_SAMPLE_RATE);
    return ESP_OK;
}

bool audio_mic_ready(void) { return s_mic_ok; }

size_t audio_read_raw(int16_t *buf, size_t frames, int channels, uint32_t timeout_ticks)
{
    (void)timeout_ticks;                               // esp_codec_dev_read blocks per frame period
    if (!s_mic_ok || channels != MIC_CHANNELS) return 0;
    if (esp_codec_dev_read(s_mic, buf, (int)(frames * channels * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    // Per-channel level for diagnostics (cheap: every 8th block).
    static int n;
    if ((n++ & 7) == 0) {
        for (int ch = 0; ch < channels; ch++) {
            double sum = 0;
            for (size_t i = 0; i < frames; i++) {
                double v = buf[i * channels + ch];
                sum += v * v;
            }
            double rms = sqrt(sum / frames) / 32768.0;
            s_level_db[ch] = rms > 1e-5 ? (float)(20.0 * log10(rms)) : -96.0f;
        }
    }
    return frames;
}

int audio_input_levels(float *db, int max)
{
    int n = max < MIC_CHANNELS ? max : MIC_CHANNELS;
    for (int i = 0; i < n; i++) db[i] = s_level_db[i];
    return s_mic_ok ? n : 0;
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

// Gemini's 24 kHz audio -> the board's 16 kHz (see components/dsp, unit-tested).
static dsp_resampler_t s_rs;
static int16_t s_rs_out[DSP_RESAMPLE_MAX_OUT(6000)];

esp_err_t audio_play(const int16_t *pcm, size_t samples)
{
    if (!s_play) return ESP_ERR_INVALID_STATE;
    while (samples) {
        size_t n = samples < 6000 ? samples : 6000;   // bounded output buffer
        size_t out = dsp_resample_24_16(&s_rs, pcm, n, s_rs_out);
        if (out) audio_play_16k(s_rs_out, out);
        pcm += n;
        samples -= n;
    }
    return ESP_OK;
}

void audio_flush_playback(void)
{
    s_flush = true;
    dsp_resampler_reset(&s_rs);
}

bool audio_is_playing(void)
{
    return s_play && (s_writing || xStreamBufferBytesAvailable(s_play) > 0);
}

void audio_set_volume(int volume)
{
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, volume < 0 ? 0 : volume > 100 ? 100 : volume);
}

void audio_wait_idle(int timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(300));                    // let queued prompts start
    for (int t = 0; t < timeout_ms && audio_is_playing(); t += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(250));                    // tail of the speaker's echo
}
