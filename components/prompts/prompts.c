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
#include "dsp.h"
#include "esp_random.h"

static const char *TAG = "prompts";

// Raw 16 kHz mono s16le clips embedded from components/prompts/clips/
extern const uint8_t ready_pcm_start[] asm("_binary_ready_ulaw_start");
extern const uint8_t ready_pcm_end[]   asm("_binary_ready_ulaw_end");
extern const uint8_t need_wifi_pcm_start[] asm("_binary_need_wifi_ulaw_start");
extern const uint8_t need_wifi_pcm_end[]   asm("_binary_need_wifi_ulaw_end");
extern const uint8_t need_api_key_pcm_start[] asm("_binary_need_api_key_ulaw_start");
extern const uint8_t need_api_key_pcm_end[]   asm("_binary_need_api_key_ulaw_end");
extern const uint8_t wifi_lost_pcm_start[] asm("_binary_wifi_lost_ulaw_start");
extern const uint8_t wifi_lost_pcm_end[]   asm("_binary_wifi_lost_ulaw_end");
extern const uint8_t wifi_back_pcm_start[] asm("_binary_wifi_back_ulaw_start");
extern const uint8_t wifi_back_pcm_end[]   asm("_binary_wifi_back_ulaw_end");
extern const uint8_t no_internet_pcm_start[] asm("_binary_no_internet_ulaw_start");
extern const uint8_t no_internet_pcm_end[]   asm("_binary_no_internet_ulaw_end");
extern const uint8_t gemini_failed_pcm_start[] asm("_binary_gemini_failed_ulaw_start");
extern const uint8_t gemini_failed_pcm_end[]   asm("_binary_gemini_failed_ulaw_end");
extern const uint8_t saved_pcm_start[] asm("_binary_saved_ulaw_start");
extern const uint8_t saved_pcm_end[]   asm("_binary_saved_ulaw_end");
extern const uint8_t settings_saved_pcm_start[] asm("_binary_settings_saved_ulaw_start");
extern const uint8_t settings_saved_pcm_end[]   asm("_binary_settings_saved_ulaw_end");
extern const uint8_t deleted_pcm_start[] asm("_binary_deleted_ulaw_start");
extern const uint8_t deleted_pcm_end[]   asm("_binary_deleted_ulaw_end");
extern const uint8_t error_pcm_start[] asm("_binary_error_ulaw_start");
extern const uint8_t error_pcm_end[]   asm("_binary_error_ulaw_end");
extern const uint8_t speaker_test_pcm_start[] asm("_binary_speaker_test_ulaw_start");
extern const uint8_t speaker_test_pcm_end[]   asm("_binary_speaker_test_ulaw_end");

extern const uint8_t wake_ack_1_pcm_start[] asm("_binary_wake_ack_1_ulaw_start");
extern const uint8_t wake_ack_1_pcm_end[]   asm("_binary_wake_ack_1_ulaw_end");
extern const uint8_t wake_ack_2_pcm_start[] asm("_binary_wake_ack_2_ulaw_start");
extern const uint8_t wake_ack_2_pcm_end[]   asm("_binary_wake_ack_2_ulaw_end");
extern const uint8_t wake_ack_3_pcm_start[] asm("_binary_wake_ack_3_ulaw_start");
extern const uint8_t wake_ack_3_pcm_end[]   asm("_binary_wake_ack_3_ulaw_end");
extern const uint8_t not_ready_pcm_start[] asm("_binary_not_ready_ulaw_start");
extern const uint8_t not_ready_pcm_end[]   asm("_binary_not_ready_ulaw_end");

extern const uint8_t mic_test_start_pcm_start[] asm("_binary_mic_test_start_ulaw_start");
extern const uint8_t mic_test_start_pcm_end[]   asm("_binary_mic_test_start_ulaw_end");
extern const uint8_t mic_test_hello_pcm_start[] asm("_binary_mic_test_hello_ulaw_start");
extern const uint8_t mic_test_hello_pcm_end[]   asm("_binary_mic_test_hello_ulaw_end");
extern const uint8_t mic_test_nothing_pcm_start[] asm("_binary_mic_test_nothing_ulaw_start");
extern const uint8_t mic_test_nothing_pcm_end[]   asm("_binary_mic_test_nothing_ulaw_end");

extern const uint8_t battery_low_pcm_start[] asm("_binary_battery_low_ulaw_start");
extern const uint8_t battery_low_pcm_end[]   asm("_binary_battery_low_ulaw_end");
extern const uint8_t battery_critical_pcm_start[] asm("_binary_battery_critical_ulaw_start");
extern const uint8_t battery_critical_pcm_end[]   asm("_binary_battery_critical_ulaw_end");

extern const uint8_t timer_done_pcm_start[] asm("_binary_timer_done_ulaw_start");
extern const uint8_t timer_done_pcm_end[]   asm("_binary_timer_done_ulaw_end");
extern const uint8_t alarm_ringing_pcm_start[] asm("_binary_alarm_ringing_ulaw_start");
extern const uint8_t alarm_ringing_pcm_end[]   asm("_binary_alarm_ringing_ulaw_end");
extern const uint8_t update_started_pcm_start[] asm("_binary_update_started_ulaw_start");
extern const uint8_t update_started_pcm_end[]   asm("_binary_update_started_ulaw_end");
extern const uint8_t update_done_pcm_start[] asm("_binary_update_done_ulaw_start");
extern const uint8_t update_done_pcm_end[]   asm("_binary_update_done_ulaw_end");
extern const uint8_t update_failed_pcm_start[] asm("_binary_update_failed_ulaw_start");
extern const uint8_t update_failed_pcm_end[]   asm("_binary_update_failed_ulaw_end");
extern const uint8_t update_offer_pcm_start[] asm("_binary_update_offer_ulaw_start");
extern const uint8_t update_offer_pcm_end[]   asm("_binary_update_offer_ulaw_end");

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
    { timer_done_pcm_start, timer_done_pcm_end },
    { alarm_ringing_pcm_start, alarm_ringing_pcm_end },
    { update_started_pcm_start, update_started_pcm_end },
    { update_done_pcm_start, update_done_pcm_end },
    { update_failed_pcm_start, update_failed_pcm_end },
    { update_offer_pcm_start, update_offer_pcm_end },
};

// Ids in enum order (must match prompt_t and tools/prompts.tsv).
static const char *const IDS[PROMPT_COUNT] = {
    "ready", "need_wifi", "need_api_key", "wifi_lost", "wifi_back", "no_internet",
    "gemini_failed", "saved", "settings_saved", "deleted", "error", "speaker_test",
    "wake_ack_1", "wake_ack_2", "wake_ack_3", "not_ready",
    "mic_test_start", "mic_test_hello", "mic_test_nothing",
    "battery_low", "battery_critical", "timer_done", "alarm_ringing",
    "update_started", "update_done", "update_failed", "update_offer",
};

// The phrase list itself is embedded so the device can re-record lines in Gemini's voice.
extern const char tsv_start[] asm("_binary_prompts_tsv_start");
extern const char tsv_end[]   asm("_binary_prompts_tsv_end");
static char *s_text[PROMPT_COUNT];

extern const char builtin_voice_start[] asm("_binary_voice_txt_start");
extern const char builtin_voice_end[]   asm("_binary_voice_txt_end");
static char s_builtin_voice[48];

static QueueHandle_t s_queue;
static char s_recorded_dir[64];                    // "" = built-in clips

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

const char *prompts_builtin_voice(void)
{
    if (!s_builtin_voice[0]) {
        size_t n = builtin_voice_end - builtin_voice_start;
        if (n >= sizeof(s_builtin_voice)) n = sizeof(s_builtin_voice) - 1;
        memcpy(s_builtin_voice, builtin_voice_start, n);
        s_builtin_voice[n] = 0;
        s_builtin_voice[strcspn(s_builtin_voice, "\r\n")] = 0;
    }
    return s_builtin_voice;
}

void prompts_set_recorded_dir(const char *dir)
{
    strlcpy(s_recorded_dir, dir ? dir : "", sizeof(s_recorded_dir));
}

void prompts_recorded_path(const char *dir, prompt_t p, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s.ulaw", dir, prompts_id(p));
}

bool prompts_is_recorded(prompt_t p)
{
    if (!s_recorded_dir[0] || p >= PROMPT_COUNT) return false;
    char path[96];
    prompts_recorded_path(s_recorded_dir, p, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/** Play a recording made in Gemini's voice, if there is one. */
static bool play_recorded(prompt_t p)
{
    if (!s_recorded_dir[0]) return false;
    char path[96];
    prompts_recorded_path(s_recorded_dir, p, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static uint8_t enc[1024];
    static int16_t pcm[1024];
    size_t n, total = 0;
    while ((n = fread(enc, 1, sizeof(enc), f)) > 0) {
        for (size_t i = 0; i < n; i++) pcm[i] = dsp_ulaw_decode(enc[i]);   // stored at 8 bits/sample to fit two voices
        audio_play_16k(pcm, n);
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
        // Built-in clips are mu-law (8 bits/sample) to keep the firmware small.
        static int16_t pcm[1024];
        const uint8_t *enc = CLIPS[p].start;
        size_t samples = (size_t)(CLIPS[p].end - CLIPS[p].start);
        ESP_LOGI(TAG, "say %s (built-in %s, %.1f s)", IDS[p], prompts_builtin_voice(), samples / 16000.0f);
        for (size_t off = 0; off < samples; off += 1024) {
            size_t n = samples - off < 1024 ? samples - off : 1024;
            for (size_t i = 0; i < n; i++) pcm[i] = dsp_ulaw_decode(enc[off + i]);
            audio_play_16k(pcm, n);
        }
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
