// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "audio.h"
#include "settings.h"
#include "prompts.h"
#include "esp_random.h"

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

extern const uint8_t wake_ack_1_pcm_start[] asm("_binary_wake_ack_1_pcm_start");
extern const uint8_t wake_ack_1_pcm_end[]   asm("_binary_wake_ack_1_pcm_end");
extern const uint8_t wake_ack_2_pcm_start[] asm("_binary_wake_ack_2_pcm_start");
extern const uint8_t wake_ack_2_pcm_end[]   asm("_binary_wake_ack_2_pcm_end");
extern const uint8_t wake_ack_3_pcm_start[] asm("_binary_wake_ack_3_pcm_start");
extern const uint8_t wake_ack_3_pcm_end[]   asm("_binary_wake_ack_3_pcm_end");
extern const uint8_t not_ready_pcm_start[] asm("_binary_not_ready_pcm_start");
extern const uint8_t not_ready_pcm_end[]   asm("_binary_not_ready_pcm_end");

extern const uint8_t mic_test_start_pcm_start[] asm("_binary_mic_test_start_pcm_start");
extern const uint8_t mic_test_start_pcm_end[]   asm("_binary_mic_test_start_pcm_end");
extern const uint8_t mic_test_hello_pcm_start[] asm("_binary_mic_test_hello_pcm_start");
extern const uint8_t mic_test_hello_pcm_end[]   asm("_binary_mic_test_hello_pcm_end");
extern const uint8_t mic_test_nothing_pcm_start[] asm("_binary_mic_test_nothing_pcm_start");
extern const uint8_t mic_test_nothing_pcm_end[]   asm("_binary_mic_test_nothing_pcm_end");

extern const uint8_t battery_low_pcm_start[] asm("_binary_battery_low_pcm_start");
extern const uint8_t battery_low_pcm_end[]   asm("_binary_battery_low_pcm_end");
extern const uint8_t battery_critical_pcm_start[] asm("_binary_battery_critical_pcm_start");
extern const uint8_t battery_critical_pcm_end[]   asm("_binary_battery_critical_pcm_end");

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
    { wake_ack_1_pcm_start, wake_ack_1_pcm_end },
    { wake_ack_2_pcm_start, wake_ack_2_pcm_end },
    { wake_ack_3_pcm_start, wake_ack_3_pcm_end },
    { not_ready_pcm_start, not_ready_pcm_end },
    { mic_test_start_pcm_start, mic_test_start_pcm_end },
    { mic_test_hello_pcm_start, mic_test_hello_pcm_end },
    { mic_test_nothing_pcm_start, mic_test_nothing_pcm_end },
    { battery_low_pcm_start, battery_low_pcm_end },
    { battery_critical_pcm_start, battery_critical_pcm_end },
};

// Ids in enum order (must match prompt_t and tools/prompts.tsv).
static const char *const IDS[PROMPT_COUNT] = {
    "ready", "need_wifi", "need_api_key", "wifi_lost", "wifi_back", "no_internet",
    "gemini_failed", "saved", "settings_saved", "deleted", "error", "speaker_test",
    "wake_ack_1", "wake_ack_2", "wake_ack_3", "not_ready",
    "mic_test_start", "mic_test_hello", "mic_test_nothing",
    "battery_low", "battery_critical",
};

// The phrase list itself is embedded so the device can re-record lines in Gemini's voice.
extern const char tsv_start[] asm("_binary_prompts_tsv_start");
extern const char tsv_end[]   asm("_binary_prompts_tsv_end");
static char *s_text[PROMPT_COUNT];

static QueueHandle_t s_queue;
static volatile bool s_use_recorded;

static void parse_tsv(void)
{
    const char *p = tsv_start;
    while (p < tsv_end) {
        const char *eol = memchr(p, '\n', tsv_end - p);
        if (!eol) eol = tsv_end;
        const char *tab = memchr(p, '\t', eol - p);
        if (*p != '#' && tab) {
            for (int i = 0; i < PROMPT_COUNT; i++) {
                if ((size_t)(tab - p) == strlen(IDS[i]) && strncmp(p, IDS[i], tab - p) == 0) {
                    s_text[i] = strndup(tab + 1, eol - tab - 1);
                }
            }
        }
        p = eol + 1;
    }
}

const char *prompts_id(prompt_t p)   { return p < PROMPT_COUNT ? IDS[p] : ""; }
const char *prompts_text(prompt_t p) { return p < PROMPT_COUNT && s_text[p] ? s_text[p] : ""; }
const char *prompts_list_raw(size_t *len) { *len = tsv_end - tsv_start; return tsv_start; }
void prompts_use_recorded(bool on)   { s_use_recorded = on; }

void prompts_recorded_path(prompt_t p, char *buf, size_t len)
{
    snprintf(buf, len, PROMPTS_RECORDED_DIR "/%s.pcm", prompts_id(p));
}

/** Play a recording made in Gemini's voice, if there is one. */
static bool play_recorded(prompt_t p)
{
    if (!s_use_recorded) return false;
    char path[64];
    prompts_recorded_path(p, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static int16_t chunk[1024];
    size_t n, total = 0;
    while ((n = fread(chunk, sizeof(int16_t), 1024, f)) > 0) {
        audio_play_16k(chunk, n);
        total += n;
    }
    fclose(f);
    ESP_LOGI(TAG, "say %s (recorded, %.1f s)", IDS[p], total / 16000.0f);
    return total > 0;
}

static void prompts_task(void *arg)
{
    prompt_t p;
    for (;;) {
        xQueueReceive(s_queue, &p, portMAX_DELAY);
        if (play_recorded(p)) continue;
        const int16_t *pcm = (const int16_t *)CLIPS[p].start;
        size_t samples = (size_t)(CLIPS[p].end - CLIPS[p].start) / sizeof(int16_t);
        ESP_LOGI(TAG, "say %s (built-in, %.1f s)", IDS[p], samples / 16000.0f);
        audio_play_16k(pcm, samples);
    }
}

esp_err_t prompts_init(void)
{
    parse_tsv();
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

void prompts_wake_ack(void)
{
    // Always spoken: it's the answer to "Jarvis", not a status message.
    prompts_say_always((prompt_t)(PROMPT_WAKE_ACK_1 + esp_random() % 3));
}
