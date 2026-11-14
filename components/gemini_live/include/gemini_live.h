// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "cJSON.h"

/** Called for each functionCall in a toolCall. Return a cJSON response object (caller frees). */
typedef cJSON *(*gemini_tool_handler_t)(const char *name, const cJSON *args);

typedef struct {
    gemini_tool_handler_t tool_handler;
    const char *system_instruction_extra;   // e.g. current time + upcoming appointments
} gemini_live_config_t;

#define GEMINI_LIVE_CONFIG_DEFAULT() { .tool_handler = NULL, .system_instruction_extra = NULL }

/** Open the BidiGenerateContent WebSocket, send setup, start mic/speaker tasks. */
esp_err_t gemini_live_start(const gemini_live_config_t *cfg);
esp_err_t gemini_live_stop(void);
/** Send a text turn (used for spoken reminders). */
esp_err_t gemini_live_send_text(const char *text);
