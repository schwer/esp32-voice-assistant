// SPDX-License-Identifier: MIT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "audio.h"
#include "wakeword.h"

static const char *TAG = "wakeword";

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_data;
static wakeword_config_t s_cfg;
static volatile bool s_conversing;

static void feed_task(void *arg)
{
    const int chunk = s_afe->get_feed_chunksize(s_data);     // samples per channel
    const int channels = s_afe->get_feed_channel_num(s_data);
    int16_t *buf = heap_caps_malloc(chunk * channels * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(buf);

    for (;;) {
        // Interleaved frames laid out as described by CONFIG_VA_AFE_INPUT_FORMAT.
        if (audio_read_raw(buf, chunk, channels, portMAX_DELAY) == (size_t)chunk) {
            s_afe->feed(s_data, buf);
        }
    }
}

static void fetch_task(void *arg)
{
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_data);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }
        if (!s_conversing && res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected");
            if (s_cfg.on_wake) {
                s_cfg.on_wake(s_cfg.ctx);
            }
            continue;
        }
        if (s_conversing && s_cfg.on_audio) {
            s_cfg.on_audio(res->data, res->data_size / sizeof(int16_t),
                           res->vad_state == VAD_SPEECH, s_cfg.ctx);
        }
    }
}

esp_err_t wakeword_start(const wakeword_config_t *cfg)
{
    s_cfg = *cfg;

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "no speech models found in the 'model' partition");
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *afe_cfg = afe_config_init(CONFIG_VA_AFE_INPUT_FORMAT, models,
                                            AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_cfg) {
        return ESP_FAIL;
    }
    afe_cfg->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX,
                                                     CONFIG_VA_WAKENET_KEYWORD);
    if (!afe_cfg->wakenet_model_name) {
        ESP_LOGE(TAG, "wake word model '%s' not found; enable it in menuconfig "
                      "(ESP Speech Recognition > Load Multiple Wake Words)",
                 CONFIG_VA_WAKENET_KEYWORD);
        afe_config_free(afe_cfg);
        return ESP_ERR_NOT_FOUND;
    }
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg = afe_config_check(afe_cfg);

    s_afe = esp_afe_handle_from_config(afe_cfg);
    s_data = s_afe->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_data) {
        return ESP_FAIL;
    }
    s_afe->print_pipeline(s_data);
    ESP_LOGI(TAG, "listening for '%s'", CONFIG_VA_WAKENET_KEYWORD);

    xTaskCreatePinnedToCore(feed_task, "afe_feed", 8 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 8 * 1024, NULL, 5, NULL, 1);
    return ESP_OK;
}

void wakeword_listen(void)
{
    s_conversing = false;
    s_afe->enable_wakenet(s_data);
}

void wakeword_converse(void)
{
    s_afe->disable_wakenet(s_data);
    s_conversing = true;
}
