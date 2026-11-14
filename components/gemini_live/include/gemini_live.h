// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/** Called for each functionCall in a toolCall. Return a cJSON response object (caller frees). */
typedef cJSON *(*gemini_tool_handler_t)(const char *name, const cJSON *args);

typedef enum {
    GEMINI_STATE_CONNECTING,
    GEMINI_STATE_READY,          // setupComplete received: listening for the user
    GEMINI_STATE_MODEL_SPEAKING, // audio from the model is playing
    GEMINI_STATE_CLOSED,
    GEMINI_STATE_ERROR,
} gemini_state_t;

typedef void (*gemini_state_cb_t)(gemini_state_t state, void *ctx);

typedef struct {
    gemini_tool_handler_t tool_handler;
    gemini_state_cb_t on_state;          // optional; e.g. LED feedback
    void *ctx;
    const char *system_instruction_extra;   // e.g. current time + upcoming appointments
} gemini_live_config_t;

#define GEMINI_LIVE_CONFIG_DEFAULT() { .tool_handler = NULL, .on_state = NULL, .ctx = NULL, .system_instruction_extra = NULL }

/** Open the BidiGenerateContent WebSocket, send setup, start mic/speaker tasks. */
esp_err_t gemini_live_start(const gemini_live_config_t *cfg);
esp_err_t gemini_live_stop(void);
/** Stream 16 kHz mono mic PCM (buffered until the session is ready). */
esp_err_t gemini_live_send_audio(const int16_t *pcm, size_t samples);
/** True once setupComplete has been received. */
bool gemini_live_ready(void);
/** Send a text turn (used for spoken reminders). */
esp_err_t gemini_live_send_text(const char *text);
