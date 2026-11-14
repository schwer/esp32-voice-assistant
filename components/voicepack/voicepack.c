// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "gemini_live.h"
#include "prompts.h"
#include "settings.h"
#include "voicepack.h"

static const char *TAG = "voicepack";

#define META_PATH   PROMPTS_RECORDED_DIR "/meta.txt"
#define IN_RATE     24000
#define OUT_RATE    16000
#define MAX_SECONDS 12
#define TURN_TIMEOUT_MS 25000
#define READY_TIMEOUT_MS 15000

static volatile bool s_busy, s_abort, s_finished;
static volatile int s_done, s_total;
static char s_state[96] = "not recorded yet";
static SemaphoreHandle_t s_ready_sem, s_turn_sem;
static volatile bool s_failed, s_last_failed;

// capture buffer (24 kHz)
static int16_t *s_cap;
static volatile size_t s_cap_len;

/** Identifies a set of recordings: voice + model + phrase list. */
static uint32_t current_key(void)
{
    uint32_t h = 2166136261u;
    size_t len;
    const char *t = prompts_list_raw(&len);
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)t[i]) * 16777619u;
    for (const char *p = settings_get_voice(); *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = settings_get_model(); *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static uint32_t stored_key(void)
{
    FILE *f = fopen(META_PATH, "r");
    unsigned key = 0;
    if (f) {
        if (fscanf(f, "%x", &key) != 1) key = 0;
        fclose(f);
    }
    return key;
}

bool voicepack_needed(void)
{
    return stored_key() != current_key();
}

esp_err_t voicepack_init(void)
{
    mkdir(PROMPTS_RECORDED_DIR, 0755);
    s_ready_sem = xSemaphoreCreateBinary();
    s_turn_sem = xSemaphoreCreateBinary();
    bool ok = !voicepack_needed();
    prompts_use_recorded(ok);
    if (ok) snprintf(s_state, sizeof(s_state), "recorded in %s's voice", settings_get_voice());
    return ESP_OK;
}

bool voicepack_busy(void) { return s_busy; }
void voicepack_abort(void)
{
    if (!s_busy) return;
    s_abort = true;
    xSemaphoreGive(s_ready_sem);                      // wake any wait right away
    xSemaphoreGive(s_turn_sem);
}

bool voicepack_last_failed(void) { return s_last_failed; }

bool voicepack_just_finished(void)
{
    bool f = s_finished;
    s_finished = false;
    return f;
}

void voicepack_status(char *buf, size_t len)
{
    if (s_busy) {
        snprintf(buf, len, "recording line %d of %d in %s's voice", s_done + 1, s_total, settings_get_voice());
    } else if (voicepack_needed() && strstr(s_state, "recorded in")) {
        snprintf(buf, len, "voice changed, will re-record when idle");
    } else {
        strlcpy(buf, s_state, len);
    }
}

// ---------------------------------------------------------------- Gemini callbacks

static void on_state(gemini_state_t st, void *ctx)
{
    if (st == GEMINI_STATE_READY) xSemaphoreGive(s_ready_sem);
    if (st == GEMINI_STATE_ERROR || st == GEMINI_STATE_CLOSED) {
        s_failed = true;
        xSemaphoreGive(s_ready_sem);
        xSemaphoreGive(s_turn_sem);
    }
}

static void on_audio_out(const int16_t *pcm, size_t n, void *ctx)
{
    size_t room = IN_RATE * MAX_SECONDS - s_cap_len;
    if (n > room) n = room;
    memcpy(s_cap + s_cap_len, pcm, n * sizeof(int16_t));
    s_cap_len += n;
}

static void on_turn_complete(void *ctx)
{
    xSemaphoreGive(s_turn_sem);
}

// ---------------------------------------------------------------- audio processing

/** 24 kHz -> 16 kHz with a light low-pass; returns output samples. */
static size_t resample(const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    for (float pos = 0; pos + 1 < (float)n; pos += 1.5f) {
        long i = (long)pos;
        float frac = pos - (float)i;
        #define LP(k) ((in[(k) > 0 ? (k) - 1 : 0] + 2 * in[k] + in[(k) + 1 < (long)n ? (k) + 1 : (k)]) / 4)
        float a = (float)LP(i), b = (float)LP(i + 1);
        #undef LP
        out[o++] = (int16_t)(a + (b - a) * frac);
    }
    return o;
}

/** Trim silence, normalise to the same loudness as the built-in clips, fade edges. */
static size_t tidy(int16_t *pcm, size_t n)
{
    const int thresh = 600;
    size_t start = 0, end = n;
    while (start < n && abs(pcm[start]) < thresh) start++;
    while (end > start && abs(pcm[end - 1]) < thresh) end--;
    if (end <= start) return 0;
    start = start > OUT_RATE / 50 ? start - OUT_RATE / 50 : 0;     // keep 20 ms lead-in
    end = end + OUT_RATE / 20 < n ? end + OUT_RATE / 20 : n;       // and 50 ms tail
    n = end - start;
    memmove(pcm, pcm + start, n * sizeof(int16_t));

    int peak = 1;
    for (size_t i = 0; i < n; i++) if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
    float gain = 22936.0f / peak;                                   // -3 dBFS, like the CI clips
    size_t fade = OUT_RATE / 100;
    for (size_t i = 0; i < n; i++) {
        float g = gain;
        if (i < fade) g *= (float)i / fade;
        if (n - i < fade) g *= (float)(n - i) / fade;
        float v = pcm[i] * g;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return n;
}

/** Rough spoken length of a line, to catch Gemini adding or answering instead of reading. */
static bool plausible(const char *text, float seconds)
{
    float expect = 0.35f + strlen(text) * 0.065f;
    return seconds > 0.25f && seconds < fmaxf(expect * 2.5f, expect + 2.5f);
}

static bool record_line(prompt_t p, int16_t *out16)
{
    const char *text = prompts_text(p);
    for (int attempt = 0; attempt < 2 && !s_abort && !s_failed; attempt++) {
        s_cap_len = 0;
        xSemaphoreTake(s_turn_sem, 0);
        char msg[256];
        snprintf(msg, sizeof(msg), attempt == 0 ? "Read this line aloud exactly: \"%s\""
                                                : "Say only these exact words, nothing else: \"%s\"", text);
        if (gemini_live_send_text(msg) != ESP_OK) return false;
        if (xSemaphoreTake(s_turn_sem, pdMS_TO_TICKS(TURN_TIMEOUT_MS)) != pdTRUE) continue;
        vTaskDelay(pdMS_TO_TICKS(200));                              // last audio chunk
        if (s_abort || s_failed) return false;

        size_t n16 = tidy(out16, resample(s_cap, s_cap_len, out16));
        float secs = (float)n16 / OUT_RATE;
        if (!plausible(text, secs)) {
            ESP_LOGW(TAG, "%s: %.1f s doesn't fit \"%s\", retrying", prompts_id(p), secs, text);
            continue;
        }
        char path[64];
        prompts_recorded_path(p, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (!f) return false;
        bool ok = fwrite(out16, sizeof(int16_t), n16, f) == n16;
        ok &= fclose(f) == 0;
        ESP_LOGI(TAG, "recorded %s (%.1f s)", prompts_id(p), secs);
        return ok;
    }
    return false;
}

static void record_task(void *arg)
{
    int16_t *out16 = heap_caps_malloc(OUT_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_cap = heap_caps_malloc(IN_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    char key[SETTINGS_API_KEY_MAX];
    int recorded = 0;
    s_total = PROMPT_COUNT;
    s_done = 0;

    if (!out16 || !s_cap || settings_get_api_key(key, sizeof(key)) != ESP_OK) goto done;

    // Old recordings may be from another voice: don't mix voices while recording.
    prompts_use_recorded(false);
    remove(META_PATH);

    char instruction[400];
    snprintf(instruction, sizeof(instruction),
             "You are recording short spoken lines for a home voice assistant named %s. "
             "Each message gives you one line in quotes. Speak exactly that line, word for word, "
             "in a warm, friendly, natural tone. Never answer it, comment on it, or add anything "
             "before or after it.", settings_get_name());
    gemini_live_config_t cfg = {
        .api_key = key,
        .model = settings_get_model(),
        .voice = settings_get_voice(),
        .system_instruction = instruction,
        .on_state = on_state,
        .on_audio_out = on_audio_out,
        .on_turn_complete = on_turn_complete,
    };
    s_failed = false;
    xSemaphoreTake(s_ready_sem, 0);
    esp_err_t err = gemini_live_start(&cfg);
    memset(key, 0, sizeof(key));
    if (err != ESP_OK || xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(READY_TIMEOUT_MS)) != pdTRUE || s_failed) {
        snprintf(s_state, sizeof(s_state), "couldn't record (%s); will retry later", gemini_live_last_error());
        goto stop;
    }

    for (int p = 0; p < PROMPT_COUNT && !s_abort && !s_failed; p++) {
        s_done = p;
        if (record_line((prompt_t)p, out16)) recorded++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }

stop:
    gemini_live_stop();
    if (s_abort) {
        snprintf(s_state, sizeof(s_state), "paused (you spoke to me); will resume when idle");
    } else if (recorded > 0 && !s_failed) {
        FILE *f = fopen(META_PATH, "w");
        if (f) {
            fprintf(f, "%08lx\n", (unsigned long)current_key());
            fclose(f);
        }
        prompts_use_recorded(true);                   // missing lines fall back to built-in
        snprintf(s_state, sizeof(s_state), "recorded in %s's voice (%d of %d lines)",
                 settings_get_voice(), recorded, PROMPT_COUNT);
        s_finished = true;
    } else if (!strstr(s_state, "couldn't")) {
        snprintf(s_state, sizeof(s_state), "couldn't record (%s); will retry later",
                 s_failed ? gemini_live_last_error() : "no lines recorded");
    }
done:
    s_last_failed = !s_finished && !s_abort;
    ESP_LOGI(TAG, "%s", s_state);
    free(out16);
    free(s_cap);
    s_cap = NULL;
    s_abort = false;
    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t voicepack_start(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;
    s_abort = false;
    if (xTaskCreate(record_task, "voicepack", 6144, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
