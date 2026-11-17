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
#include "esp_tls_errors.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "audio.h"
#include "gemini_live.h"
#include "gemini_proto.h"

static const char *TAG = "gemini";

#define URL_BASE "wss://generativelanguage.googleapis.com/ws/" \
                 "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
#define RATE            16000
#define SEND_SAMPLES    1600                        // 100 ms per realtimeInput message
#define PREROLL_SAMPLES (RATE * 10)                 // speech captured before the session is ready
                                                    // (incl. Wi-Fi reconnect in battery saver)
#define CONNECT_TIMEOUT_MS 12000

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
static gemini_scratch_t s_scratch;                  // audio decode buffer
static volatile bool s_turn_done;                   // turnComplete seen, waiting for playback to end
static gemini_transcript_t s_transcript;
static char s_said[GEMINI_TRANSCRIPT_MAX];          // what Gemini said recently (echo check)
static volatile bool s_echo;                        // heard itself: no barge-in for this session
static cJSON *s_tool_msg;                           // toolResponse being assembled

const char *gemini_live_last_error(void) { return s_error; }
bool gemini_live_echo_detected(void) { return s_echo; }
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
    size_t n = gemini_build_audio(pcm, samples, s_json_out, GEMINI_AUDIO_MSG_MAX(SEND_SAMPLES));
    if (n) send_json(s_json_out, n);
}

esp_err_t gemini_live_send_audio(const int16_t *pcm, size_t samples, bool speech)
{
    if (!s_active || !s_lock) return ESP_ERR_INVALID_STATE;
    if (speech) touch();

    // While Gemini talks, the mic only stays open when interruptions are on and the
    // echo canceller is doing its job (otherwise Gemini would interrupt itself).
    if ((!s_cfg.barge_in || s_echo) && audio_is_playing()) {
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
    gemini_setup_t setup = {
        .model = s_cfg.model,
        .voice = s_cfg.voice,
        .system_instruction = s_system,
        .tools = s_cfg.tools,
        .google_search = s_cfg.google_search,
        // Transcripts also feed the echo check, so ask for them whenever barge-in is on.
        .transcription = s_cfg.on_transcript != NULL || s_cfg.barge_in,
        .proactive_audio = s_cfg.proactive_audio,
    };
    s_cfg.tools = NULL;                              // consumed by gemini_build_setup
    char *json = gemini_build_setup(&setup);
    if (json) {
        ESP_LOGI(TAG, "setup: model %s, voice %s, search %s, interruptions %s, only-when-spoken-to %s (%u bytes)",
                 s_cfg.model, s_cfg.voice, s_cfg.google_search ? "on" : "off", s_cfg.barge_in ? "on" : "off",
                 s_cfg.proactive_audio ? "on" : "off", (unsigned)strlen(json));
        send_json(json, strlen(json));
        free(json);
    }
}

esp_err_t gemini_live_send_text(const char *text)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char *json = gemini_build_text_turn(text);
    int sent = json ? send_json(json, strlen(json)) : -1;
    free(json);
    touch();
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------- receiving

static void flush_transcripts(void)
{
    if (s_cfg.on_transcript) {
        if (s_transcript.user[0]) s_cfg.on_transcript("user", s_transcript.user, s_cfg.ctx);
        if (s_transcript.assistant[0]) s_cfg.on_transcript("assistant", s_transcript.assistant, s_cfg.ctx);
    }
    gemini_transcript_reset(&s_transcript);
}

// Server message handlers (parsing itself: gemini_proto.c, unit-tested on the host).

static void h_setup_complete(void *ctx)
{
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

static void h_audio(const int16_t *pcm, size_t samples, void *ctx)
{
    set_state(GEMINI_STATE_MODEL_SPEAKING);
    s_turn_done = false;
    if (s_cfg.on_audio_out) {
        s_cfg.on_audio_out(pcm, samples, s_cfg.ctx);    // recording
    } else {
        audio_play(pcm, samples);                       // 24 kHz -> resampled to 16 kHz
    }
    touch();
}

static void h_input_transcript(const char *t, void *ctx)
{
    gemini_transcript_append(s_transcript.user, t);
    // Echo guard: if what Gemini "heard" is mostly what it just said, the speaker is
    // leaking into the mics. Keep the mic closed while it talks for the rest of the session.
    if (s_cfg.barge_in && !s_echo && gemini_looks_like_echo(s_transcript.user, s_said)) {
        s_echo = true;
        ESP_LOGW(TAG, "Gemini heard its own voice; interruptions off for this conversation "
                      "(echo cancellation isn't working: check the microphone channel layout)");
    }
}

static void h_output_transcript(const char *t, void *ctx)
{
    gemini_transcript_append(s_transcript.assistant, t);
    gemini_transcript_append(s_said, t);
    gemini_transcript_keep_tail(s_said, 600);
}

static void h_interrupted(void *ctx)
{
    ESP_LOGI(TAG, "interrupted");
    audio_flush_playback();
}

static void h_turn_complete(void *ctx)
{
    flush_transcripts();
    s_turn_done = true;                                 // READY once playback has finished
    touch();
    if (s_cfg.on_turn_complete) s_cfg.on_turn_complete(s_cfg.ctx);
}

static void h_tool_call(const char *id, const char *name, const cJSON *args, void *ctx)
{
    if (!s_tool_msg) s_tool_msg = gemini_tool_response_new();
    cJSON *result = s_cfg.tool_handler ? s_cfg.tool_handler(name, args) : NULL;
    gemini_tool_response_add(s_tool_msg, id, name, result);
}

static void h_tool_calls_done(void *ctx)
{
    char *json = gemini_print_and_free(s_tool_msg);
    s_tool_msg = NULL;
    if (json) {
        send_json(json, strlen(json));
        free(json);
    }
    touch();
}

static void h_go_away(void *ctx)
{
    ESP_LOGW(TAG, "server will close the session soon");
}

static const gemini_handlers_t HANDLERS = {
    .setup_complete = h_setup_complete,
    .audio = h_audio,
    .input_transcript = h_input_transcript,
    .output_transcript = h_output_transcript,
    .interrupted = h_interrupted,
    .turn_complete = h_turn_complete,
    .tool_call = h_tool_call,
    .tool_calls_done = h_tool_calls_done,
    .go_away = h_go_away,
};

static void handle_message(const char *data, size_t len)
{
    if (!gemini_parse_message(data, len, &HANDLERS, &s_scratch)) {
        ESP_LOGW(TAG, "unparseable message (%u bytes)", (unsigned)len);
    }
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
            char reason[90];
            int code = gemini_parse_close((const uint8_t *)d->data_ptr, d->data_len > 0 ? d->data_len : 0,
                                          reason, sizeof(reason));
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

    case WEBSOCKET_EVENT_ERROR: {
        if (s_closing) break;
        // Say which step failed, so the diagnostics page points at the real cause.
        const esp_websocket_error_codes_t *e = &d->error_handle;
        char why[128];
        esp_err_t tls = e->esp_tls_last_esp_err;
        if (e->esp_ws_handshake_status_code >= 400) {
            snprintf(why, sizeof(why), "Gemini refused the connection (HTTP %d): check the API key and model",
                     e->esp_ws_handshake_status_code);
        } else if (e->esp_transport_sock_errno == 23 || tls == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET) {
            snprintf(why, sizeof(why), "out of network sockets (errno %d)", e->esp_transport_sock_errno);
        } else if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
            snprintf(why, sizeof(why), "couldn't look up Gemini's address (DNS)");
        } else if (tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
            snprintf(why, sizeof(why), "connecting to Gemini timed out");
        } else if (tls != ESP_OK) {
            snprintf(why, sizeof(why), "secure connection to Gemini failed (%s, errno %d)",
                     esp_err_to_name(tls), e->esp_transport_sock_errno);
        } else {
            snprintf(why, sizeof(why), "network error talking to Gemini (type %d, errno %d)",
                     (int)e->error_type, e->esp_transport_sock_errno);
        }
        fail(why);
        break;
    }
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
    if (!s_json_out) s_json_out = heap_caps_malloc(GEMINI_AUDIO_MSG_MAX(SEND_SAMPLES), MALLOC_CAP_SPIRAM);
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
    gemini_transcript_reset(&s_transcript);
    s_said[0] = 0;
    s_echo = false;
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
    gemini_scratch_free(&s_scratch);
    cJSON_Delete(s_tool_msg);
    s_tool_msg = NULL;
    return ESP_OK;
}
