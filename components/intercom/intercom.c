// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "audio.h"
#include "dsp.h"
#include "gemini_live.h"
#include "settings.h"
#include "storage.h"
#include "leds.h"
#include "voicepack.h"
#include "intercom.h"

static const char *TAG = "intercom";

#define DIR         STORAGE_ROOT "/intercom"
#define INDEX_PATH  DIR "/index.json"
#define IN_RATE     24000
#define MAX_SECONDS 30
#define READY_TIMEOUT_MS 15000
#define TURN_TIMEOUT_MS  30000

typedef enum { ST_IDLE, ST_WAITING, ST_PREPARING, ST_PLAYING, ST_DONE, ST_FAILED } state_t;

static SemaphoreHandle_t s_lock, s_ready_sem, s_turn_sem, s_job_sem;
static bool (*s_gate)(void);
static volatile state_t s_state;
static volatile bool s_abort, s_gemini_failed;
static volatile int s_replay_id;          // job: 0 = say s_text, else replay this message
static char s_message[160];
static int s_last_id;
static char s_text[INTERCOM_TEXT_BYTES + 1];
static int16_t *s_cap;
static volatile size_t s_cap_len;
static int16_t s_chime[16000 * 36 / 100];      // two soft notes, 0.36 s

// ---------------------------------------------------------------- index on flash

static cJSON *load_index(void)
{
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) return cJSON_CreateArray();
    char *buf = calloc(1, 8192);
    size_t n = buf ? fread(buf, 1, 8191, f) : 0;
    fclose(f);
    cJSON *a = buf ? cJSON_ParseWithLength(buf, n) : NULL;
    free(buf);
    if (!cJSON_IsArray(a)) {
        cJSON_Delete(a);
        a = cJSON_CreateArray();
    }
    return a;
}

static void save_index(cJSON *a)
{
    char *json = cJSON_PrintUnformatted(a);
    if (!json) return;
    FILE *f = fopen(INDEX_PATH ".tmp", "w");
    if (f) {
        bool ok = fputs(json, f) >= 0;
        ok &= fclose(f) == 0;
        if (ok) {
            remove(INDEX_PATH);
            rename(INDEX_PATH ".tmp", INDEX_PATH);
        }
    }
    free(json);
}

static void audio_path(int id, char *buf, size_t len)
{
    snprintf(buf, len, DIR "/%d.ulaw", id);
}

// ---------------------------------------------------------------- playback

static void make_chime(void)
{
    const size_t n = sizeof(s_chime) / sizeof(s_chime[0]), half = n / 2;
    for (size_t i = 0; i < n; i++) {
        float f = i < half ? 784.0f : 1046.5f;                 // G5 then C6
        size_t k = i < half ? i : i - half;
        float env = expf(-(float)k / (16000 * 0.09f));         // bell-like decay
        if (k < 80) env *= k / 80.0f;                          // no click
        s_chime[i] = (int16_t)(0.30f * 32767 * env * sinf(2 * 3.14159265f * f * k / 16000.0f));
    }
}

static void play_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t enc[1024];
    static int16_t pcm[1024];
    size_t n;
    while ((n = fread(enc, 1, sizeof(enc), f)) > 0) {
        for (size_t i = 0; i < n; i++) pcm[i] = dsp_ulaw_decode(enc[i]);
        audio_play_16k(pcm, n);
    }
    fclose(f);
}

static void play_message(int id)
{
    char path[48];
    audio_path(id, path, sizeof(path));
    leds_state_t before = leds_get();
    leds_set(LEDS_SPEAKING);
    s_state = ST_PLAYING;
    audio_play_16k(s_chime, sizeof(s_chime) / sizeof(s_chime[0]));
    static const int16_t gap[16000 / 4] = {0};                 // 0.25 s after the chime
    audio_play_16k(gap, sizeof(gap) / sizeof(gap[0]));
    play_file(path);
    audio_wait_idle(MAX_SECONDS * 1000);
    leds_set(before == LEDS_SPEAKING ? LEDS_OFF : before);
}

// ---------------------------------------------------------------- Gemini reads it

static void on_state(gemini_state_t st, void *ctx)
{
    if (st == GEMINI_STATE_READY) xSemaphoreGive(s_ready_sem);
    if (st == GEMINI_STATE_ERROR || st == GEMINI_STATE_CLOSED) {
        s_gemini_failed = true;
        xSemaphoreGive(s_ready_sem);
        xSemaphoreGive(s_turn_sem);
    }
}

static void on_audio_out(const int16_t *pcm, size_t n, void *ctx)
{
    if (!s_cap) return;
    size_t room = IN_RATE * MAX_SECONDS - s_cap_len;
    if (n > room) n = room;
    memcpy(s_cap + s_cap_len, pcm, n * sizeof(int16_t));
    s_cap_len += n;
}

static void on_turn_complete(void *ctx) { xSemaphoreGive(s_turn_sem); }

static void fail(const char *why)
{
    snprintf(s_message, sizeof(s_message), "%s", why);
    s_state = ST_FAILED;
    ESP_LOGW(TAG, "%s", why);
}

/** Record s_text in Gemini's voice into DIR/<id>.ulaw. Returns seconds, or <0. */
static float record(int id)
{
    char key[SETTINGS_API_KEY_MAX];
    if (settings_get_api_key(key, sizeof(key)) != ESP_OK) { fail("No Gemini API key saved (setup page)."); return -1; }
    char instruction[360];
    snprintf(instruction, sizeof(instruction),
             "You are %s, a home voice assistant, used as an intercom. Each message gives you an "
             "announcement in quotes. Say exactly that text, word for word, clearly and naturally, as "
             "if announcing it to people in the room. Never answer it, comment on it, or add anything.",
             settings_get_name());
    gemini_live_config_t cfg = {
        .api_key = key,
        .model = settings_get_model(),
        .voice = voicepack_conversation_voice(),
        .system_instruction = instruction,
        .on_state = on_state,
        .on_audio_out = on_audio_out,
        .on_turn_complete = on_turn_complete,
    };
    s_gemini_failed = false;
    s_cap_len = 0;
    s_cap = heap_caps_malloc(IN_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);   // 1.4 MB, while recording
    xSemaphoreTake(s_ready_sem, 0);
    xSemaphoreTake(s_turn_sem, 0);
    esp_err_t err = gemini_live_start(&cfg);
    memset(key, 0, sizeof(key));
    if (err != ESP_OK || xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(READY_TIMEOUT_MS)) != pdTRUE || s_gemini_failed) {
        char why[160];
        snprintf(why, sizeof(why), "Couldn't reach Gemini: %s",
                 gemini_live_last_error()[0] ? gemini_live_last_error() : "no answer");
        gemini_live_stop();
        free(s_cap);
        s_cap = NULL;
        fail(why);
        return -1;
    }
    int16_t *out16 = heap_caps_malloc(DSP_RESAMPLE_MAX_OUT(IN_RATE * MAX_SECONDS) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    float secs = -1;
    for (int attempt = 0; attempt < 2 && out16 && s_cap && !s_abort && !s_gemini_failed; attempt++) {
        s_cap_len = 0;
        char *msg = malloc(INTERCOM_TEXT_BYTES + 80);
        if (!msg) break;
        snprintf(msg, INTERCOM_TEXT_BYTES + 80, attempt == 0 ? "Announce this exactly: \"%s\""
                                                           : "Say only these exact words, nothing else: \"%s\"", s_text);
        err = gemini_live_send_text(msg);
        free(msg);
        if (err != ESP_OK) break;
        if (xSemaphoreTake(s_turn_sem, pdMS_TO_TICKS(TURN_TIMEOUT_MS)) != pdTRUE) continue;
        vTaskDelay(pdMS_TO_TICKS(200));                        // last audio chunk
        if (s_abort || s_gemini_failed) break;
        dsp_resampler_t rs;
        dsp_resampler_reset(&rs);
        size_t n16 = dsp_tidy(out16, dsp_resample_24_16(&rs, s_cap, s_cap_len, out16));
        float t = (float)n16 / 16000;
        if (n16 == 0 || (attempt == 0 && !dsp_take_plausible(s_text, t))) {
            ESP_LOGW(TAG, "%.1f s doesn't fit the text, asking again", t);
            continue;
        }
        uint8_t *enc = (uint8_t *)out16;                       // encode in place (1 byte/sample)
        for (size_t i = 0; i < n16; i++) enc[i] = dsp_ulaw_encode(out16[i]);
        char path[48];
        audio_path(id, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        bool ok = f && fwrite(enc, 1, n16, f) == n16;
        if (f) ok &= fclose(f) == 0;
        if (ok) secs = t;
        break;
    }
    gemini_live_stop();
    free(out16);
    free(s_cap);
    s_cap = NULL;
    if (secs < 0) {
        if (s_abort) fail("Stopped: someone started talking to Jarvis.");
        else if (s_gemini_failed) {
            char why[160];
            snprintf(why, sizeof(why), "Gemini stopped: %s", gemini_live_last_error());
            fail(why);
        } else fail("Gemini didn't read it clearly. Try again or reword it.");
    }
    return secs;
}

// ---------------------------------------------------------------- job

static void intercom_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_job_sem, portMAX_DELAY);
        s_abort = false;
        if (s_replay_id) {
            int id = s_replay_id;
            s_replay_id = 0;
            if (s_gate && !s_gate()) { fail("Jarvis is busy (in a conversation). Try again in a moment."); continue; }
            play_message(id);
            snprintf(s_message, sizeof(s_message), "Said it again.");
            s_state = ST_DONE;
            continue;
        }
        // Wait (up to 20 s) for the board to be free: not in a conversation or test.
        s_state = ST_WAITING;
        if (voicepack_busy()) voicepack_abort();               // the user comes first; it resumes later
        int waited = 0;
        while (s_gate && !s_gate() && waited < 20000 && !s_abort) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
        while (voicepack_busy() && waited < 25000) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
        if (s_abort || (s_gate && !s_gate()) || voicepack_busy()) {
            fail("Jarvis is busy (in a conversation). Try again in a moment.");
            continue;
        }
        s_state = ST_PREPARING;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON *idx = load_index();
        int id = 1;
        const cJSON *e;
        cJSON_ArrayForEach(e, idx) {
            int n = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id"));
            if (n >= id) id = n + 1;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "message %d: \"%s\"", id, s_text);
        float secs = record(id);
        if (secs < 0) {
            cJSON_Delete(idx);
            continue;
        }
        // Save: newest first, keep INTERCOM_KEEP.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "id", id);
        cJSON_AddStringToObject(m, "text", s_text);
        cJSON_AddNumberToObject(m, "time", (double)time(NULL));
        cJSON_AddNumberToObject(m, "seconds", roundf(secs * 10) / 10);
        cJSON_InsertItemInArray(idx, 0, m);
        while (cJSON_GetArraySize(idx) > INTERCOM_KEEP) {
            cJSON *old = cJSON_DetachItemFromArray(idx, cJSON_GetArraySize(idx) - 1);
            char path[48];
            audio_path((int)cJSON_GetNumberValue(cJSON_GetObjectItem(old, "id")), path, sizeof(path));
            remove(path);
            cJSON_Delete(old);
        }
        save_index(idx);
        cJSON_Delete(idx);
        s_last_id = id;
        xSemaphoreGive(s_lock);
        play_message(id);
        snprintf(s_message, sizeof(s_message), "Said it (%.1f s).", secs);
        s_state = ST_DONE;
    }
}

// ---------------------------------------------------------------- API

esp_err_t intercom_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ready_sem = xSemaphoreCreateBinary();
    s_turn_sem = xSemaphoreCreateBinary();
    s_job_sem = xSemaphoreCreateBinary();
    if (!s_lock || !s_ready_sem || !s_turn_sem || !s_job_sem) return ESP_ERR_NO_MEM;
    mkdir(DIR, 0755);
    make_chime();
    xTaskCreate(intercom_task, "intercom", 6144, NULL, 4, NULL);
    return ESP_OK;
}

void intercom_set_gate(bool (*can_start)(void)) { s_gate = can_start; }

esp_err_t intercom_say(const char *text)
{
    if (!s_job_sem) return ESP_ERR_INVALID_STATE;
    while (text && (*text == ' ' || *text == '\n' || *text == '\t')) text++;
    size_t len = text ? strlen(text) : 0;
    if (!len || len > INTERCOM_TEXT_BYTES) return ESP_ERR_INVALID_ARG;
    if (s_state == ST_WAITING || s_state == ST_PREPARING || s_state == ST_PLAYING) return ESP_ERR_INVALID_STATE;
    // Quotes would end the quoted line we hand Gemini: use typographic ones instead.
    size_t o = 0;
    for (size_t i = 0; i < len && o < sizeof(s_text) - 1; i++) {
        char c = text[i];
        if (c == '"') c = '\'';
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        s_text[o++] = c;
    }
    s_text[o] = 0;
    s_message[0] = 0;
    s_state = ST_WAITING;
    xSemaphoreGive(s_job_sem);
    return ESP_OK;
}

esp_err_t intercom_replay(int id)
{
    if (!s_job_sem || intercom_busy() || id <= 0) return ESP_ERR_INVALID_STATE;
    char path[48];
    audio_path(id, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    s_message[0] = 0;
    s_replay_id = id;
    s_state = ST_PLAYING;                     // busy from now on
    xSemaphoreGive(s_job_sem);
    return ESP_OK;
}

esp_err_t intercom_delete(int id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *idx = load_index();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < cJSON_GetArraySize(idx); i++) {
        if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(idx, i), "id")) == id) {
            cJSON_DeleteItemFromArray(idx, i);
            char path[48];
            audio_path(id, path, sizeof(path));
            remove(path);
            save_index(idx);
            err = ESP_OK;
            break;
        }
    }
    cJSON_Delete(idx);
    xSemaphoreGive(s_lock);
    return err;
}

bool intercom_busy(void)
{
    return s_state == ST_WAITING || s_state == ST_PREPARING || s_state == ST_PLAYING;
}

void intercom_abort(void)
{
    if (s_state == ST_WAITING || s_state == ST_PREPARING) {
        s_abort = true;
        xSemaphoreGive(s_turn_sem);
    }
}

cJSON *intercom_status(void)
{
    static const char *const names[] = { "idle", "waiting", "preparing", "playing", "done", "failed" };
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", names[s_state]);
    cJSON_AddStringToObject(r, "message", s_message);
    cJSON_AddNumberToObject(r, "id", s_last_id);
    return r;
}

cJSON *intercom_list(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!s_lock) {
        cJSON_AddArrayToObject(r, "messages");
        return r;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddItemToObject(r, "messages", load_index());
    xSemaphoreGive(s_lock);
    return r;
}
