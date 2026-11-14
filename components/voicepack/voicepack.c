// SPDX-License-Identifier: MIT
//
// Layout on flash:
//   /data/voice/<Voice>/<id>.ulaw   one recorded line (16 kHz, µ-law)
//   /data/voice/<Voice>/complete    every line of the phrase list is recorded
//   /data/voice/active.txt          the complete set in use ("" = built-in clips)
//   /data/voice/redo.txt            re-record requested for this voice
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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

#define ACTIVE_PATH PROMPTS_RECORDED_DIR "/active.txt"
#define REDO_PATH   PROMPTS_RECORDED_DIR "/redo.txt"
#define IN_RATE     24000
#define OUT_RATE    16000
#define MAX_SECONDS 12
#define ATTEMPTS    3
#define TURN_TIMEOUT_MS  25000
#define READY_TIMEOUT_MS 15000

static volatile bool s_busy, s_abort, s_failed, s_last_failed, s_finished;
static volatile int s_done, s_total;
static char s_state[128];
static char s_active[32];                 // voice of the set in use ("" = built-in)
static char s_recording_voice[32];
static SemaphoreHandle_t s_ready_sem, s_turn_sem;
static int16_t *s_cap;
static volatile size_t s_cap_len;

// ---------------------------------------------------------------- files

static void voice_dir(const char *voice, char *buf, size_t len)
{
    snprintf(buf, len, PROMPTS_RECORDED_DIR "/%s", voice);
}

static void read_line(const char *path, char *buf, size_t len)
{
    buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, len, f)) buf[strcspn(buf, "\r\n")] = 0;
    fclose(f);
}

static void write_line(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool set_complete(const char *voice)
{
    char path[96];
    snprintf(path, sizeof(path), PROMPTS_RECORDED_DIR "/%s/complete", voice);
    return exists(path);
}

static bool line_recorded(const char *voice, prompt_t p)
{
    char dir[64], path[96];
    voice_dir(voice, dir, sizeof(dir));
    prompts_recorded_path(dir, p, path, sizeof(path));
    return exists(path);
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
    rmdir(dir);
}

/** Delete recorded sets other than these two voices (frees space for a new set). */
static void remove_other_sets(const char *keep1, const char *keep2)
{
    DIR *d = opendir(PROMPTS_RECORDED_DIR);
    if (!d) return;
    struct dirent *e;
    char names[8][32];
    int n = 0;
    while ((e = readdir(d)) != NULL && n < 8) {
        if (e->d_name[0] == '.' || strchr(e->d_name, '.')) continue;   // skip *.txt
        if (strcmp(e->d_name, keep1) == 0 || strcmp(e->d_name, keep2) == 0) continue;
        strlcpy(names[n++], e->d_name, sizeof(names[0]));
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char dir[64];
        voice_dir(names[i], dir, sizeof(dir));
        remove_dir(dir);
    }
}

static void activate(const char *voice)
{
    strlcpy(s_active, voice, sizeof(s_active));
    write_line(ACTIVE_PATH, voice);
    char dir[64];
    voice_dir(voice, dir, sizeof(dir));
    prompts_set_recorded_dir(voice[0] ? dir : NULL);
}

// ---------------------------------------------------------------- state

const char *voicepack_active_voice(void)
{
    return s_active[0] ? s_active : prompts_builtin_voice();
}

const char *voicepack_conversation_voice(void)
{
    // The built-in clips may not be a Gemini voice (they're made with an offline
    // TTS engine). Until a set is recorded, conversations use the selected voice.
    return s_active[0] ? s_active : settings_get_voice();
}

bool voicepack_needed(void)
{
    if (exists(REDO_PATH)) return true;
    return strcmp(s_active, settings_get_voice()) != 0;
}

bool voicepack_busy(void) { return s_busy; }
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
        snprintf(buf, len, "recording messages in %s's voice (%d of %d); until then everything stays in %s's voice",
                 s_recording_voice, s_done + 1, s_total, voicepack_active_voice());
    } else if (s_state[0]) {
        strlcpy(buf, s_state, len);
    } else if (voicepack_needed()) {
        snprintf(buf, len, "will record messages in %s's voice when online and idle", settings_get_voice());
    } else {
        snprintf(buf, len, "messages and conversations use %s's voice", voicepack_active_voice());
    }
}

esp_err_t voicepack_init(void)
{
    mkdir(PROMPTS_RECORDED_DIR, 0755);
    s_ready_sem = xSemaphoreCreateBinary();
    s_turn_sem = xSemaphoreCreateBinary();
    char v[32];
    read_line(ACTIVE_PATH, v, sizeof(v));
    activate(v[0] && set_complete(v) ? v : "");
    ESP_LOGI(TAG, "messages in %s's voice", voicepack_active_voice());
    return ESP_OK;
}

void voicepack_abort(void)
{
    if (!s_busy) return;
    s_abort = true;
    xSemaphoreGive(s_ready_sem);                      // wake any wait right away
    xSemaphoreGive(s_turn_sem);
}

void voicepack_voice_changed(void)
{
    s_state[0] = 0;
    s_last_failed = false;
    if (s_busy && strcmp(s_recording_voice, settings_get_voice()) != 0) voicepack_abort();
}

esp_err_t voicepack_rerecord(void)
{
    if (s_busy) {
        voicepack_abort();
        for (int i = 0; i < 100 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (s_busy) return ESP_ERR_TIMEOUT;
    }
    write_line(REDO_PATH, settings_get_voice());
    s_state[0] = 0;
    s_last_failed = false;
    ESP_LOGI(TAG, "re-record requested in %s's voice", settings_get_voice());
    return ESP_OK;
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

static void on_turn_complete(void *ctx) { xSemaphoreGive(s_turn_sem); }

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

/** One biquad section (direct form I). */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;

static float biquad(biquad_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

/**
 * Same treatment as the built-in clips (tools/gen_prompts.py: speakerize):
 * remove bass the small speaker can't play, +3 dB presence at 3 kHz, set the
 * speech loudness to -13 dBFS, and soft-limit peaks.
 */
static void speakerize(int16_t *pcm, size_t n)
{
    const float fs = OUT_RATE, pi = 3.14159265f;
    // 2nd-order Butterworth high-pass at 150 Hz (RBJ, Q = 0.7071)
    float w0 = 2 * pi * 150 / fs, al = sinf(w0) / (2 * 0.7071f), c = cosf(w0), a0 = 1 + al;
    biquad_t hp = { (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0 };
    // +3 dB peaking EQ at 3 kHz, Q = 1
    float A = powf(10, 3.0f / 40), w1 = 2 * pi * 3000 / fs, al1 = sinf(w1) / 2, c1 = cosf(w1), d0 = 1 + al1 / A;
    biquad_t pk = { (1 + al1 * A) / d0, -2 * c1 / d0, (1 - al1 * A) / d0, -2 * c1 / d0, (1 - al1 / A) / d0 };

    float *f = heap_caps_malloc(n * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!f) return;
    double sum = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        f[i] = biquad(&pk, biquad(&hp, pcm[i] / 32768.0f));
        if (fabsf(f[i]) > 0.02f) { sum += (double)f[i] * f[i]; cnt++; }
    }
    float rms = cnt ? sqrtf((float)(sum / cnt)) : 1e-3f;
    float gain = powf(10, -13.0f / 20) / fmaxf(rms, 1e-6f);
    const float knee = 0.6f;
    for (size_t i = 0; i < n; i++) {
        float v = f[i] * gain, m = fabsf(v);
        if (m > knee) v = copysignf(knee + 0.35f * tanhf((m - knee) / 0.35f), v);
        pcm[i] = (int16_t)(v * 32767);
    }
    free(f);
}

/** Trim silence, make it clear on the speaker, fade the edges. */
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

    speakerize(pcm, n);
    size_t fade = OUT_RATE / 100;
    for (size_t i = 0; i < fade && i < n; i++) {
        pcm[i] = (int16_t)(pcm[i] * (float)i / fade);
        pcm[n - 1 - i] = (int16_t)(pcm[n - 1 - i] * (float)i / fade);
    }
    return n;
}

/** Rough spoken length of a line, to catch Gemini answering instead of reading. */
static bool plausible(const char *text, float seconds)
{
    float expect = 0.35f + strlen(text) * 0.065f;
    return seconds > 0.25f && seconds < fmaxf(expect * 2.5f, expect + 2.5f);
}

static bool record_line(const char *dir, prompt_t p, int16_t *out16)
{
    const char *text = prompts_text(p);
    for (int attempt = 0; attempt < ATTEMPTS && !s_abort && !s_failed; attempt++) {
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
        // Encode in place (µ-law is 1 byte per sample) and write via a temp file,
        // so a line is never left half-written.
        uint8_t *enc = (uint8_t *)out16;
        for (size_t i = 0; i < n16; i++) enc[i] = prompts_ulaw_encode(out16[i]);
        char path[96], tmp[100];
        prompts_recorded_path(dir, p, path, sizeof(path));
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (!f) return false;
        bool ok = fwrite(enc, 1, n16, f) == n16;
        ok &= fclose(f) == 0;
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        }
        ESP_LOGI(TAG, "recorded %s (%.1f s)", prompts_id(p), secs);
        return ok;
    }
    return false;
}

static void record_task(void *arg)
{
    int16_t *out16 = heap_caps_malloc(OUT_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_cap = heap_caps_malloc(IN_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    char key[SETTINGS_API_KEY_MAX], dir[64];
    const char *voice = s_recording_voice;
    bool redo = exists(REDO_PATH);
    int missing = 0;
    s_failed = false;
    s_state[0] = 0;

    voice_dir(voice, dir, sizeof(dir));
    if (redo) remove_dir(dir);                        // fresh takes of every line
    remove_other_sets(voice, s_active);              // keep only the set in use + the new one
    mkdir(dir, 0755);

    // Only the lines not recorded yet (an interrupted run resumes where it stopped).
    s_total = 0;
    for (int p = 0; p < PROMPT_COUNT; p++) if (!line_recorded(voice, (prompt_t)p)) s_total++;
    s_done = 0;

    if (s_total > 0) {
        if (!out16 || !s_cap || settings_get_api_key(key, sizeof(key)) != ESP_OK) {
            snprintf(s_state, sizeof(s_state), "couldn't record: %s", out16 && s_cap ? "no API key" : "out of memory");
            goto done;
        }
        char instruction[400];
        snprintf(instruction, sizeof(instruction),
                 "You are recording short spoken lines for a home voice assistant named %s. "
                 "Each message gives you one line in quotes. Speak exactly that line, word for word, "
                 "in a warm, friendly, natural tone. Never answer it, comment on it, or add anything "
                 "before or after it.", settings_get_name());
        gemini_live_config_t cfg = {
            .api_key = key,
            .model = settings_get_model(),
            .voice = voice,
            .system_instruction = instruction,
            .on_state = on_state,
            .on_audio_out = on_audio_out,
            .on_turn_complete = on_turn_complete,
        };
        xSemaphoreTake(s_ready_sem, 0);
        esp_err_t err = gemini_live_start(&cfg);
        memset(key, 0, sizeof(key));
        if (err != ESP_OK || xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(READY_TIMEOUT_MS)) != pdTRUE || s_failed) {
            snprintf(s_state, sizeof(s_state), "couldn't record in %s's voice (%s); will retry in 10 minutes",
                     voice, gemini_live_last_error()[0] ? gemini_live_last_error() : "no answer");
            gemini_live_stop();
            goto done;
        }
        for (int p = 0; p < PROMPT_COUNT && !s_abort && !s_failed; p++) {
            if (line_recorded(voice, (prompt_t)p)) continue;
            if (!record_line(dir, (prompt_t)p, out16)) missing++;
            s_done++;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        gemini_live_stop();
    }

    if (s_abort) {
        snprintf(s_state, sizeof(s_state), "paused (you were talking to me); recording in %s's voice resumes when idle", voice);
    } else if (s_failed) {
        snprintf(s_state, sizeof(s_state), "recording in %s's voice was cut off (%s); will retry in 10 minutes",
                 voice, gemini_live_last_error());
    } else if (missing > 0) {
        snprintf(s_state, sizeof(s_state), "%d line(s) didn't record cleanly in %s's voice; will retry in 10 minutes",
                 missing, voice);
    } else {
        // Every line is recorded: switch messages and conversations over together.
        char done_path[96];
        snprintf(done_path, sizeof(done_path), "%s/complete", dir);
        write_line(done_path, voice);
        char old[32];
        strlcpy(old, s_active, sizeof(old));
        activate(voice);
        remove(REDO_PATH);
        if (old[0] && strcmp(old, voice) != 0) {
            char old_dir[64];
            voice_dir(old, old_dir, sizeof(old_dir));
            remove_dir(old_dir);
        }
        snprintf(s_state, sizeof(s_state), "messages and conversations use %s's voice", voice);
        s_finished = true;
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
    strlcpy(s_recording_voice, settings_get_voice(), sizeof(s_recording_voice));
    s_busy = true;
    s_abort = false;
    if (xTaskCreate(record_task, "voicepack", 6144, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
