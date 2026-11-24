// SPDX-License-Identifier: MIT
//
// Layout on flash:
//   /data/voice/<Voice>/<id>.ulaw   one recorded line (16 kHz, µ-law)
//   /data/voice/<Voice>/complete    every line of the phrase list is recorded
//   /data/voice/active.txt          the complete set in use ("" = built-in clips)
//   /data/voice/job.txt             a recording job: "<Voice>" + "redo" (overwrite all)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include "storage.h"
#include "dsp.h"

static const char *TAG = "voicepack";

#define ACTIVE_PATH PROMPTS_RECORDED_DIR "/active.txt"
#define JOB_PATH    PROMPTS_RECORDED_DIR "/job.txt"
#define IN_RATE     24000
#define OUT_RATE    16000
#define MAX_SECONDS 12
#define ATTEMPTS    3
#define TURN_TIMEOUT_MS  25000
#define READY_TIMEOUT_MS 15000

static volatile bool s_busy, s_abort, s_failed, s_finished;
static volatile int s_done, s_total;
static char s_state[320];                 // last outcome (empty while nothing to report)
static char s_error[128];                 // why the last job stopped, if it failed
static char s_active[32];                 // voice of the set in use ("" = built-in)
static char s_job_voice[32];              // voice being recorded
static SemaphoreHandle_t s_ready_sem, s_turn_sem;
static int16_t *s_cap;
static volatile size_t s_cap_len;

// ---------------------------------------------------------------- files

static bool exists(const char *path);

// Recorded sets live in <root>/voice/<Voice>: on the microSD card (STORAGE_SD_DIR) when
// there is one, else in flash (PROMPTS_RECORDED_DIR). active.txt and job.txt stay in flash.
#define SD_VOICE_DIR STORAGE_SD_DIR "/voice"

/** Where a voice's set is (card first, then flash); a new set goes where new recordings go. */
static void voice_dir(const char *voice, char *buf, size_t len)
{
    if (storage_sd_ready()) {
        snprintf(buf, len, SD_VOICE_DIR "/%.31s", voice);           // voice names are short
        if (exists(buf)) return;
    }
    snprintf(buf, len, PROMPTS_RECORDED_DIR "/%.31s", voice);
    if (exists(buf)) return;
    snprintf(buf, len, "%s/voice/%.31s", storage_recordings_root(), voice);
}

static void read_lines(const char *path, char *l1, size_t n1, char *l2, size_t n2)
{
    l1[0] = 0;
    if (l2) l2[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(l1, n1, f)) l1[strcspn(l1, "\r\n")] = 0;
    if (l2 && fgets(l2, n2, f)) l2[strcspn(l2, "\r\n")] = 0;
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
    char dir[64], path[96];
    voice_dir(voice, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/complete", dir);
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
    static const char *const roots[] = { PROMPTS_RECORDED_DIR, SD_VOICE_DIR };
    for (int r = 0; r < 2; r++) {
        if (r == 1 && !storage_sd_ready()) break;
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *e;
        char names[8][32];
        int n = 0;
        while ((e = readdir(d)) != NULL && n < 8) {
            if (e->d_name[0] == '.' || strchr(e->d_name, '.')) continue;   // skip *.txt
            if (strcasecmp(e->d_name, keep1) == 0 || strcasecmp(e->d_name, keep2) == 0) continue;
            strlcpy(names[n++], e->d_name, sizeof(names[0]));
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char dir[64];
            snprintf(dir, sizeof(dir), "%s/%.31s", roots[r], names[i]);
            remove_dir(dir);
        }
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
    // The built-in clips aren't a Gemini voice (they're made with an offline TTS
    // engine). Until a set is recorded, conversations use the selected voice.
    return s_active[0] ? s_active : settings_get_voice();
}

bool voicepack_matches_selected(void)
{
    return s_active[0] && strcmp(s_active, settings_get_voice()) == 0;
}

bool voicepack_pending(void) { return exists(JOB_PATH); }
const char *voicepack_progress_voice(void) { return s_job_voice; }
bool voicepack_busy(void) { return s_busy; }

bool voicepack_just_finished(void)
{
    bool f = s_finished;
    s_finished = false;
    return f;
}

void voicepack_progress(voicepack_progress_t *p)
{
    memset(p, 0, sizeof(*p));
    p->busy = s_busy;
    p->pending = voicepack_pending();
    p->done = s_done;
    p->total = s_total;
    strlcpy(p->voice, s_busy || p->pending ? s_job_voice : voicepack_active_voice(), sizeof(p->voice));
    strlcpy(p->active_voice, voicepack_active_voice(), sizeof(p->active_voice));
    strlcpy(p->error, s_error, sizeof(p->error));
    voicepack_status(p->status, sizeof(p->status));
}

void voicepack_status(char *buf, size_t len)
{
    if (s_busy) {
        snprintf(buf, len, "recording messages in %s's voice: %d of %d", s_job_voice, s_done, s_total);
    } else if (voicepack_pending()) {
        snprintf(buf, len, "recording in %s's voice paused%s%s; it continues when the board is free",
                 s_job_voice, s_error[0] ? ": " : "", s_error);
    } else if (s_state[0]) {
        strlcpy(buf, s_state, len);
    } else if (!s_active[0]) {
        snprintf(buf, len, "built-in voice: save the setup page to record messages in %s's voice",
                 settings_get_voice());
    } else {
        snprintf(buf, len, "messages and conversations use %s's voice", s_active);
    }
}

esp_err_t voicepack_init(void)
{
    mkdir(PROMPTS_RECORDED_DIR, 0755);
    s_ready_sem = xSemaphoreCreateBinary();
    s_turn_sem = xSemaphoreCreateBinary();
    char v[32];
    read_lines(ACTIVE_PATH, v, sizeof(v), NULL, 0);
    activate(v[0] && set_complete(v) ? v : "");
    read_lines(JOB_PATH, s_job_voice, sizeof(s_job_voice), NULL, 0);
    ESP_LOGI(TAG, "messages in %s's voice%s", voicepack_active_voice(),
             voicepack_pending() ? " (a recording job is waiting)" : "");
    return ESP_OK;
}

void voicepack_abort(void)
{
    if (!s_busy) return;
    s_abort = true;
    xSemaphoreGive(s_ready_sem);                      // wake any wait right away
    xSemaphoreGive(s_turn_sem);
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
// Resampling, clean-up and the take check live in components/dsp (unit-tested).

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

        dsp_resampler_t rs;
        dsp_resampler_reset(&rs);
        size_t n16 = dsp_tidy(out16, dsp_resample_24_16(&rs, s_cap, s_cap_len, out16));
        float secs = (float)n16 / OUT_RATE;
        if (!dsp_take_plausible(text, secs)) {
            ESP_LOGW(TAG, "%s: %.1f s doesn't fit \"%s\", retrying", prompts_id(p), secs, text);
            continue;
        }
        // Encode in place (µ-law is 1 byte per sample) and write via a temp file,
        // so a line is never left half-written.
        uint8_t *enc = (uint8_t *)out16;
        for (size_t i = 0; i < n16; i++) enc[i] = dsp_ulaw_encode(out16[i]);
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


// ---------------------------------------------------------------- job

static void record_task(void *arg)
{
    int16_t *out16 = heap_caps_malloc(DSP_RESAMPLE_MAX_OUT(IN_RATE * MAX_SECONDS) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_cap = heap_caps_malloc(IN_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    char key[SETTINGS_API_KEY_MAX], dir[64], mode[16];
    read_lines(JOB_PATH, s_job_voice, sizeof(s_job_voice), mode, sizeof(mode));
    const char *voice = s_job_voice;
    bool redo = strcmp(mode, "redo") == 0;
    int missing = 0;
    s_failed = false;
    s_error[0] = 0;

    remove_other_sets(voice, s_active);              // keep only the set in use + the new one
    voice_dir(voice, dir, sizeof(dir));              // a new set goes to the microSD card if there is one
    {
        char parent[64];
        strlcpy(parent, dir, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        if (strncmp(parent, STORAGE_SD_DIR, strlen(STORAGE_SD_DIR)) == 0) mkdir(STORAGE_SD_DIR, 0755);
        mkdir(parent, 0755);
    }
    mkdir(dir, 0755);
    ESP_LOGI(TAG, "recording %s's voice into %s", voice, dir);

    // Re-record: overwrite every line, tracked by "redo-<n>" progress so it can resume.
    int redo_from = 0;
    if (strncmp(mode, "redo-", 5) == 0) {
        redo = true;
        redo_from = atoi(mode + 5);
    }
    prompt_t todo[PROMPT_COUNT];
    int n_todo = 0;
    for (int p = redo ? redo_from : 0; p < PROMPT_COUNT; p++) {
        if (redo || !line_recorded(voice, (prompt_t)p)) todo[n_todo++] = (prompt_t)p;
    }
    s_total = PROMPT_COUNT;
    s_done = PROMPT_COUNT - n_todo;

    if (n_todo > 0) {
        if (!out16 || !s_cap || settings_get_api_key(key, sizeof(key)) != ESP_OK) {
            strlcpy(s_error, out16 && s_cap ? "no API key saved" : "out of memory", sizeof(s_error));
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
            snprintf(s_error, sizeof(s_error), "couldn't reach Gemini (%s)",
                     gemini_live_last_error()[0] ? gemini_live_last_error() : "no answer");
            gemini_live_stop();
            goto done;
        }
        for (int i = 0; i < n_todo && !s_abort && !s_failed; i++) {
            if (!record_line(dir, todo[i], out16)) {
                missing++;
                continue;
            }
            s_done++;
            if (redo) {                               // remember progress across interruptions
                char job[64];
                snprintf(job, sizeof(job), "%s\nredo-%d", voice, (int)todo[i] + 1);
                write_line(JOB_PATH, job);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        gemini_live_stop();
        if (s_failed && !s_abort) {
            snprintf(s_error, sizeof(s_error), "the connection to Gemini dropped (%s)", gemini_live_last_error());
        }
    }

    if (s_abort) {
        strlcpy(s_error, "you were talking to me", sizeof(s_error));
    } else if (missing > 0 && !s_error[0]) {
        snprintf(s_error, sizeof(s_error), "%d line(s) didn't record cleanly", missing);
    }

    bool all = !s_abort && !s_failed && missing == 0;
    for (int p = 0; all && p < PROMPT_COUNT; p++) if (!line_recorded(voice, (prompt_t)p)) all = false;
    if (all) {
        // Every line is recorded: switch messages and conversations over together.
        char done_path[96];
        snprintf(done_path, sizeof(done_path), "%s/complete", dir);
        write_line(done_path, voice);
        char old[32];
        strlcpy(old, s_active, sizeof(old));
        activate(voice);
        if (old[0] && strcmp(old, voice) != 0) {
            char old_dir[64];
            voice_dir(old, old_dir, sizeof(old_dir));
            remove_dir(old_dir);
        }
        remove(JOB_PATH);
        s_done = s_total;
        snprintf(s_state, sizeof(s_state), "finished: messages and conversations now use %s's voice", voice);
        s_finished = true;
    } else if (!s_abort && s_error[0]) {
        // Failed (not interrupted): drop the job; the user can save again to retry.
        remove(JOB_PATH);
        snprintf(s_state, sizeof(s_state), "couldn't finish recording in %s's voice: %s. Save the setup page to try again.",
                 voice, s_error);
    }
done:
    if (!s_finished && !s_abort && s_error[0] && exists(JOB_PATH)) {
        remove(JOB_PATH);
        snprintf(s_state, sizeof(s_state), "couldn't record in %s's voice: %s. Save the setup page to try again.",
                 voice, s_error);
    }
    ESP_LOGI(TAG, "%s", s_state[0] ? s_state : s_error);
    free(out16);
    free(s_cap);
    s_cap = NULL;
    s_abort = false;
    s_busy = false;
    vTaskDelete(NULL);
}

static bool (*s_gate)(void);
void voicepack_set_gate(bool (*can_start)(void)) { s_gate = can_start; }

static esp_err_t start_job(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (s_gate && !s_gate()) return ESP_OK;          // pending; resumed when the device is free
    s_busy = true;
    s_abort = false;
    s_state[0] = 0;
    if (xTaskCreate(record_task, "voicepack", 6144, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voicepack_request(bool redo)
{
    // A job for another voice is replaced; stop it first.
    if (s_busy) {
        voicepack_abort();
        for (int i = 0; i < 100 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(50));
        if (s_busy) return ESP_ERR_TIMEOUT;
    }
    char job[64];
    snprintf(job, sizeof(job), "%s\n%s", settings_get_voice(), redo ? "redo" : "fill");
    write_line(JOB_PATH, job);
    strlcpy(s_job_voice, settings_get_voice(), sizeof(s_job_voice));
    s_state[0] = 0;
    s_error[0] = 0;
    s_done = 0;
    s_total = PROMPT_COUNT;
    ESP_LOGI(TAG, "recording requested in %s's voice%s", s_job_voice, redo ? " (all lines again)" : "");
    return start_job();
}

esp_err_t voicepack_resume(void)
{
    return voicepack_pending() ? start_job() : ESP_ERR_NOT_FOUND;
}
