// SPDX-License-Identifier: MIT
//
// Gemini Live API over WebSocket. Protocol (as used by Google's examples):
//   -> {"setup": {model, generationConfig{responseModalities, speechConfig}, systemInstruction,
//                 tools, inputAudioTranscription, outputAudioTranscription,
//                 contextWindowCompression}}
//   <- {"setupComplete": {}}
//   -> {"realtimeInput": {"audio": {"mimeType": "audio/pcm;rate=16000", "data": <base64>}}}
//   <- {"serverContent": {"modelTurn": {"parts": [{"inlineData": {"data": <base64 24 kHz PCM>}}]},
//                         "inputTranscription": {...}, "outputTranscription": {...},
//                         "interrupted": true, "turnComplete": true}}
//   <- {"toolCall": {"functionCalls": [{"id", "name", "args"}]}}
//   -> {"toolResponse": {"functionResponses": [{"id", "name", "response"}]}}
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "audio.h"
#include "gemini_live.h"

static const char *TAG = "gemini";

#define URL_BASE "wss://generativelanguage.googleapis.com/ws/" \
                 "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
#define RATE            16000
#define SEND_SAMPLES    1600                        // 100 ms per realtimeInput message
#define PREROLL_SAMPLES (RATE * 10)                 // speech captured before the session is ready
                                                    // (incl. Wi-Fi reconnect in battery saver)
#define CONNECT_TIMEOUT_MS 12000
#define TRANSCRIPT_MAX  2048

static gemini_live_config_t s_cfg;
static char *s_system;                              // copy of the system instruction
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_lock;                    // guards send buffers / preroll
static volatile bool s_ready, s_active, s_closing;
static volatile gemini_state_t s_state;
static char s_error[128];
static TaskHandle_t s_monitor;

// upload
static int16_t *s_send;                             // SEND_SAMPLES accumulator
static size_t s_send_len;
static int16_t *s_preroll;
static size_t s_preroll_len;
static char *s_json_out;                            // realtimeInput message buffer
static volatile int64_t s_last_activity_us;

// download
static char *s_rx;
static size_t s_rx_cap;
static uint8_t *s_pcm;
static size_t s_pcm_cap;
static volatile bool s_turn_done;                   // turnComplete seen, waiting for playback to end
static char s_user_text[TRANSCRIPT_MAX], s_asst_text[TRANSCRIPT_MAX];

const char *gemini_live_last_error(void) { return s_error; }
bool gemini_live_ready(void) { return s_ready; }

static void set_state(gemini_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    if (s_cfg.on_state) s_cfg.on_state(st, s_cfg.ctx);
}

static void fail(const char *why)
{
    if (!s_active) return;
    strlcpy(s_error, why, sizeof(s_error));
    ESP_LOGE(TAG, "%s", why);
    s_active = false;
    set_state(s_ready ? GEMINI_STATE_CLOSED : GEMINI_STATE_ERROR);
}

static void touch(void) { s_last_activity_us = esp_timer_get_time(); }

// ---------------------------------------------------------------- sending

static int send_json(const char *json, size_t len)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return -1;
    return esp_websocket_client_send_text(s_ws, json, (int)len, pdMS_TO_TICKS(2000));
}

/** Send one block of 16 kHz PCM as a realtimeInput message. Caller holds s_lock. */
static void send_pcm_locked(const int16_t *pcm, size_t samples)
{
    static const char head[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    static const char tail[] = "\"}}}";
    size_t hl = sizeof(head) - 1, b64 = 0;
    memcpy(s_json_out, head, hl);
    size_t cap = 4 * ((SEND_SAMPLES * 2 + 2) / 3) + 1;
    if (mbedtls_base64_encode((unsigned char *)s_json_out + hl, cap, &b64,
                              (const unsigned char *)pcm, samples * sizeof(int16_t)) != 0) {
        return;
    }
    memcpy(s_json_out + hl + b64, tail, sizeof(tail));
    send_json(s_json_out, hl + b64 + sizeof(tail) - 1);
}

esp_err_t gemini_live_send_audio(const int16_t *pcm, size_t samples, bool speech)
{
    if (!s_active || !s_lock) return ESP_ERR_INVALID_STATE;
    if (speech) touch();

    // Half duplex unless barge-in is enabled: don't send what the speaker is
    // playing back into the mics (Gemini would interrupt itself).
    if (!s_cfg.barge_in && audio_is_playing()) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready) {
        size_t room = PREROLL_SAMPLES - s_preroll_len;
        size_t n = samples < room ? samples : room;
        memcpy(s_preroll + s_preroll_len, pcm, n * sizeof(int16_t));
        s_preroll_len += n;
    } else {
        while (samples) {
            size_t n = SEND_SAMPLES - s_send_len;
            if (n > samples) n = samples;
            memcpy(s_send + s_send_len, pcm, n * sizeof(int16_t));
            s_send_len += n;
            pcm += n;
            samples -= n;
            if (s_send_len == SEND_SAMPLES) {
                send_pcm_locked(s_send, SEND_SAMPLES);
                s_send_len = 0;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void send_setup(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    char model[96];
    snprintf(model, sizeof(model), "models/%s", s_cfg.model);
    cJSON_AddStringToObject(setup, "model", model);

    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *mod = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON_AddItemToArray(mod, cJSON_CreateString("AUDIO"));
    cJSON *voice = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(gen, "speechConfig"), "voiceConfig"),
        "prebuiltVoiceConfig");
    cJSON_AddStringToObject(voice, "voiceName", s_cfg.voice);

    cJSON *sys = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON *parts = cJSON_AddArrayToObject(sys, "parts");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", s_system ? s_system : "");
    cJSON_AddItemToArray(parts, part);

    cJSON *tools = s_cfg.tools ? s_cfg.tools : cJSON_CreateArray();
    s_cfg.tools = NULL;                              // now owned by root (or freed below)
    if (s_cfg.google_search) {
        cJSON *gs = cJSON_CreateObject();
        cJSON_AddObjectToObject(gs, "googleSearch");
        cJSON_AddItemToArray(tools, gs);
    }
    if (cJSON_GetArraySize(tools) > 0) {
        cJSON_AddItemToObject(setup, "tools", tools);
    } else {
        cJSON_Delete(tools);
    }
    if (s_cfg.on_transcript) {
        cJSON_AddObjectToObject(setup, "inputAudioTranscription");
        cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    }
    // Keep long conversations affordable: older turns slide out of the context.
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(setup, "contextWindowCompression"), "slidingWindow");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        ESP_LOGI(TAG, "setup: model %s, voice %s, search %s (%u bytes)", model, s_cfg.voice,
                 s_cfg.google_search ? "on" : "off", (unsigned)strlen(json));
        send_json(json, strlen(json));
        free(json);
    }
}

esp_err_t gemini_live_send_text(const char *text)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    cJSON *msg = cJSON_CreateObject();
    cJSON *cc = cJSON_AddObjectToObject(msg, "clientContent");
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(cc, "turns"), turn);
    cJSON_AddBoolToObject(cc, "turnComplete", true);
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    int sent = json ? send_json(json, strlen(json)) : -1;
    free(json);
    touch();
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------- receiving

static void append_text(char *buf, const char *text)
{
    size_t n = strlen(buf);
    strlcpy(buf + n, text, TRANSCRIPT_MAX - n);
}

static void flush_transcripts(void)
{
    if (s_cfg.on_transcript) {
        if (s_user_text[0]) s_cfg.on_transcript("user", s_user_text, s_cfg.ctx);
        if (s_asst_text[0]) s_cfg.on_transcript("assistant", s_asst_text, s_cfg.ctx);
    }
    s_user_text[0] = s_asst_text[0] = 0;
}

static void play_base64(const char *b64)
{
    size_t len = strlen(b64), out = 0;
    size_t need = len / 4 * 3 + 3;
    if (need > s_pcm_cap) {
        free(s_pcm);
        s_pcm = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        s_pcm_cap = s_pcm ? need : 0;
        if (!s_pcm) return;
    }
    if (mbedtls_base64_decode(s_pcm, s_pcm_cap, &out, (const unsigned char *)b64, len) == 0 && out >= 2) {
        if (s_cfg.on_audio_out) {
            s_cfg.on_audio_out((const int16_t *)s_pcm, out / 2, s_cfg.ctx);   // recording
        } else {
            audio_play((const int16_t *)s_pcm, out / 2);   // 24 kHz -> resampled to 16 kHz
        }
    }
}

static void handle_tool_call(const cJSON *tool_call)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON *resp = cJSON_AddArrayToObject(cJSON_AddObjectToObject(msg, "toolResponse"), "functionResponses");
    const cJSON *fc;
    cJSON_ArrayForEach(fc, cJSON_GetObjectItem(tool_call, "functionCalls")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "name"));
        if (!name) continue;
        cJSON *result = s_cfg.tool_handler ? s_cfg.tool_handler(name, cJSON_GetObjectItem(fc, "args")) : NULL;
        if (!result) {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "error", "no handler");
        }
        cJSON *r = cJSON_CreateObject();
        if (id) cJSON_AddStringToObject(r, "id", id);
        cJSON_AddStringToObject(r, "name", name);
        cJSON_AddItemToObject(r, "response", result);
        cJSON_AddItemToArray(resp, r);
    }
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (json) {
        send_json(json, strlen(json));
        free(json);
    }
    touch();
}

static void handle_message(const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable message (%u bytes)", (unsigned)len);
        return;
    }

    if (cJSON_GetObjectItem(root, "setupComplete")) {
        ESP_LOGI(TAG, "session ready");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ready = true;
        // Send what the user said while we were connecting.
        for (size_t off = 0; off < s_preroll_len; off += SEND_SAMPLES) {
            size_t n = s_preroll_len - off < SEND_SAMPLES ? s_preroll_len - off : SEND_SAMPLES;
            send_pcm_locked(s_preroll + off, n);
        }
        s_preroll_len = 0;
        xSemaphoreGive(s_lock);
        touch();
        set_state(GEMINI_STATE_READY);
    }

    const cJSON *tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (tool_call) handle_tool_call(tool_call);

    const cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
    if (sc) {
        const cJSON *part;
        cJSON_ArrayForEach(part, cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "modelTurn"), "parts")) {
            const char *b64 = cJSON_GetStringValue(
                cJSON_GetObjectItem(cJSON_GetObjectItem(part, "inlineData"), "data"));
            if (b64) {
                set_state(GEMINI_STATE_MODEL_SPEAKING);
                s_turn_done = false;
                play_base64(b64);
                touch();
            }
        }
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "inputTranscription"), "text"));
        if (t) append_text(s_user_text, t);
        t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "outputTranscription"), "text"));
        if (t) append_text(s_asst_text, t);

        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "interrupted"))) {
            ESP_LOGI(TAG, "interrupted");
            audio_flush_playback();
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "turnComplete"))) {
            flush_transcripts();
            s_turn_done = true;                      // READY once playback has finished
            touch();
            if (s_cfg.on_turn_complete) s_cfg.on_turn_complete(s_cfg.ctx);
        }
    }
    if (cJSON_GetObjectItem(root, "goAway")) {
        ESP_LOGW(TAG, "server will close the session soon");
    }
    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        send_setup();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08) {                    // close frame: 2-byte code + reason
            char reason[90] = "";
            int code = d->data_len >= 2 ? ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1] : 0;
            if (d->data_len > 2) {
                int n = d->data_len - 2 < (int)sizeof(reason) - 1 ? d->data_len - 2 : (int)sizeof(reason) - 1;
                memcpy(reason, d->data_ptr + 2, n);
                reason[n] = 0;
            }
            if (!s_closing) {
                char why[sizeof(reason) + 24];
                snprintf(why, sizeof(why), "%s (code %d)", reason[0] ? reason : "server closed the session", code);
                fail(why);
            }
            break;
        }
        if (d->op_code != 0x01 && d->op_code != 0x02 && d->op_code != 0x00) break;
        if (d->payload_len <= 0) break;
        if (d->payload_offset == 0 && (size_t)d->payload_len + 1 > s_rx_cap) {
            free(s_rx);
            s_rx_cap = (size_t)d->payload_len + 1;
            s_rx = heap_caps_malloc(s_rx_cap, MALLOC_CAP_SPIRAM);
            if (!s_rx) { s_rx_cap = 0; break; }
        }
        if (!s_rx || d->payload_offset + d->data_len > (int)s_rx_cap - 1) break;
        memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
        if (d->payload_offset + d->data_len >= d->payload_len) {
            s_rx[d->payload_len] = 0;
            handle_message(s_rx, d->payload_len);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (!s_closing) fail(s_ready ? "connection closed" : "couldn't connect to Gemini");
        break;

    case WEBSOCKET_EVENT_ERROR:
        if (!s_closing) fail("network error talking to Gemini");
        break;
    }
}

// ---------------------------------------------------------------- lifecycle

/** Watches for: connect timeout, end of playback after a turn, idle timeout. */
static void monitor_task(void *arg)
{
    int64_t start = esp_timer_get_time();
    while (s_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        if (!s_ready && now - start > CONNECT_TIMEOUT_MS * 1000LL) {
            fail("Gemini didn't answer in time");
            break;
        }
        if (s_ready && s_turn_done && !audio_is_playing()) {
            s_turn_done = false;
            touch();                                 // the idle clock starts after the reply
            set_state(GEMINI_STATE_READY);
        }
        if (audio_is_playing()) touch();
        if (s_ready && s_cfg.idle_timeout_s > 0 &&
            now - s_last_activity_us > s_cfg.idle_timeout_s * 1000000LL) {
            ESP_LOGI(TAG, "no one has spoken for %d s, ending", s_cfg.idle_timeout_s);
            s_active = false;
            set_state(GEMINI_STATE_CLOSED);
        }
    }
    s_monitor = NULL;
    vTaskDelete(NULL);
}

static bool alloc_buffers(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_send) s_send = heap_caps_malloc(SEND_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_preroll) s_preroll = heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_json_out) s_json_out = heap_caps_malloc(4 * ((SEND_SAMPLES * 2 + 2) / 3) + 128, MALLOC_CAP_SPIRAM);
    return s_lock && s_send && s_preroll && s_json_out;
}

esp_err_t gemini_live_start(const gemini_live_config_t *cfg)
{
    gemini_live_stop();
    if (!cfg->api_key || !cfg->api_key[0]) {
        strlcpy(s_error, "no API key saved", sizeof(s_error));
        cJSON_Delete(cfg->tools);
        return ESP_ERR_INVALID_ARG;
    }
    if (!alloc_buffers()) {
        cJSON_Delete(cfg->tools);
        return ESP_ERR_NO_MEM;
    }
    s_cfg = *cfg;
    free(s_system);
    s_system = cfg->system_instruction ? strdup(cfg->system_instruction) : NULL;
    s_error[0] = 0;
    s_ready = false;
    s_closing = false;
    s_turn_done = false;
    s_send_len = 0;
    if (!cfg->keep_buffered_audio) s_preroll_len = 0;
    s_user_text[0] = s_asst_text[0] = 0;
    s_state = (gemini_state_t)-1;
    touch();

    size_t url_len = strlen(URL_BASE) + strlen(cfg->api_key) + 1;
    char *url = malloc(url_len);
    if (!url) return ESP_ERR_NO_MEM;
    snprintf(url, url_len, "%s%s", URL_BASE, cfg->api_key);   // never logged

    esp_websocket_client_config_t ws = {
        .uri = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 16 * 1024,
        .task_stack = 12 * 1024,
        .task_prio = 6,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 15,
    };
    s_ws = esp_websocket_client_init(&ws);
    memset(url, 0, url_len);
    free(url);
    if (!s_ws) {
        cJSON_Delete(s_cfg.tools);
        s_cfg.tools = NULL;
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);

    s_active = true;
    set_state(GEMINI_STATE_CONNECTING);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        s_active = false;
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        cJSON_Delete(s_cfg.tools);
        s_cfg.tools = NULL;
        return ESP_FAIL;
    }
    xTaskCreate(monitor_task, "gemini_mon", 3072, NULL, 4, &s_monitor);
    return ESP_OK;
}

esp_err_t gemini_live_stop(void)
{
    s_closing = true;
    s_active = false;
    s_ready = false;
    while (s_monitor) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_ws) {
        if (esp_websocket_client_is_connected(s_ws)) {
            esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    flush_transcripts();
    cJSON_Delete(s_cfg.tools);
    s_cfg.tools = NULL;
    free(s_rx);
    s_rx = NULL;
    s_rx_cap = 0;
    return ESP_OK;
}
