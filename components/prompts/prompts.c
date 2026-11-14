// SPDX-License-Identifier: MIT
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "audio.h"
#include "settings.h"
#include "prompts.h"

static const char *TAG = "prompts";

// Raw 16 kHz mono s16le clips embedded from components/prompts/clips/
extern const uint8_t ready_pcm_start[] asm("_binary_ready_pcm_start");
extern const uint8_t ready_pcm_end[]   asm("_binary_ready_pcm_end");
extern const uint8_t need_wifi_pcm_start[] asm("_binary_need_wifi_pcm_start");
extern const uint8_t need_wifi_pcm_end[]   asm("_binary_need_wifi_pcm_end");
extern const uint8_t need_api_key_pcm_start[] asm("_binary_need_api_key_pcm_start");
extern const uint8_t need_api_key_pcm_end[]   asm("_binary_need_api_key_pcm_end");
extern const uint8_t wifi_lost_pcm_start[] asm("_binary_wifi_lost_pcm_start");
extern const uint8_t wifi_lost_pcm_end[]   asm("_binary_wifi_lost_pcm_end");
extern const uint8_t wifi_back_pcm_start[] asm("_binary_wifi_back_pcm_start");
extern const uint8_t wifi_back_pcm_end[]   asm("_binary_wifi_back_pcm_end");
extern const uint8_t no_internet_pcm_start[] asm("_binary_no_internet_pcm_start");
extern const uint8_t no_internet_pcm_end[]   asm("_binary_no_internet_pcm_end");
extern const uint8_t gemini_failed_pcm_start[] asm("_binary_gemini_failed_pcm_start");
extern const uint8_t gemini_failed_pcm_end[]   asm("_binary_gemini_failed_pcm_end");
extern const uint8_t saved_pcm_start[] asm("_binary_saved_pcm_start");
extern const uint8_t saved_pcm_end[]   asm("_binary_saved_pcm_end");
extern const uint8_t settings_saved_pcm_start[] asm("_binary_settings_saved_pcm_start");
extern const uint8_t settings_saved_pcm_end[]   asm("_binary_settings_saved_pcm_end");
extern const uint8_t deleted_pcm_start[] asm("_binary_deleted_pcm_start");
extern const uint8_t deleted_pcm_end[]   asm("_binary_deleted_pcm_end");
extern const uint8_t error_pcm_start[] asm("_binary_error_pcm_start");
extern const uint8_t error_pcm_end[]   asm("_binary_error_pcm_end");
extern const uint8_t speaker_test_pcm_start[] asm("_binary_speaker_test_pcm_start");
extern const uint8_t speaker_test_pcm_end[]   asm("_binary_speaker_test_pcm_end");

static const struct { const uint8_t *start, *end; } CLIPS[PROMPT_COUNT] = {
    { ready_pcm_start, ready_pcm_end },
    { need_wifi_pcm_start, need_wifi_pcm_end },
    { need_api_key_pcm_start, need_api_key_pcm_end },
    { wifi_lost_pcm_start, wifi_lost_pcm_end },
    { wifi_back_pcm_start, wifi_back_pcm_end },
    { no_internet_pcm_start, no_internet_pcm_end },
    { gemini_failed_pcm_start, gemini_failed_pcm_end },
    { saved_pcm_start, saved_pcm_end },
    { settings_saved_pcm_start, settings_saved_pcm_end },
    { deleted_pcm_start, deleted_pcm_end },
    { error_pcm_start, error_pcm_end },
    { speaker_test_pcm_start, speaker_test_pcm_end },
};

static QueueHandle_t s_queue;

static void prompts_task(void *arg)
{
    prompt_t p;
    for (;;) {
        xQueueReceive(s_queue, &p, portMAX_DELAY);
        const int16_t *pcm = (const int16_t *)CLIPS[p].start;
        size_t samples = (size_t)(CLIPS[p].end - CLIPS[p].start) / sizeof(int16_t);
        ESP_LOGI(TAG, "say #%d (%.1f s)", (int)p, samples / 16000.0f);
        audio_play_16k(pcm, samples);
    }
}

esp_err_t prompts_init(void)
{
    s_queue = xQueueCreate(4, sizeof(prompt_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    xTaskCreate(prompts_task, "prompts", 3072, NULL, 5, NULL);
    return ESP_OK;
}

void prompts_say_always(prompt_t p)
{
    if (!s_queue || p >= PROMPT_COUNT) return;
    xQueueSend(s_queue, &p, 0);          // drop if several are already waiting
}

void prompts_say(prompt_t p)
{
    if (settings_get_spoken_status()) prompts_say_always(p);
}
