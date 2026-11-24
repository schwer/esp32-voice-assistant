// SPDX-License-Identifier: MIT
//
// Intercom: a message typed on the Intercom page (or a phone) is spoken by Jarvis in
// his Gemini voice, after a short chime. Each message is kept on flash as audio, so
// "Say again" replays it instantly, even without Gemini. The last 10 are kept.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#define INTERCOM_TEXT_MAX 300                       // characters (the page limits it)
#define INTERCOM_TEXT_BYTES (INTERCOM_TEXT_MAX * 3)  // as UTF-8
#define INTERCOM_KEEP     10

esp_err_t intercom_init(void);
/** Gemini (and the speaker) may only be used when this says the board is free. */
void intercom_set_gate(bool (*can_start)(void));

/** Speak a new message (queued; returns at once). ESP_ERR_INVALID_STATE if one is
 *  already being prepared, ESP_ERR_INVALID_ARG if the text is empty or too long. */
esp_err_t intercom_say(const char *text);
/** Play a saved message again. */
esp_err_t intercom_replay(int id);
esp_err_t intercom_delete(int id);

/** Preparing or playing a message (Gemini in use). */
bool intercom_busy(void);
/** Stop preparing now (the user said "Jarvis"); the message fails with a reason. */
void intercom_abort(void);

/** {"state": "idle|waiting|preparing|playing|done|failed", "message": "...", "id": n} */
cJSON *intercom_status(void);
/** {"messages": [{"id", "text", "time", "seconds"}...]} newest first. */
cJSON *intercom_list(void);
