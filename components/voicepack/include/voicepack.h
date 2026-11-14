// SPDX-License-Identifier: MIT
//
// One voice, everywhere: the device's own lines (wake-word answers, status messages,
// tests) are recorded at runtime by asking Gemini Live to read tools/prompts.tsv in
// the voice used for conversations, and overwrite the built-in clips.
//
// A new set is recorded next to the current one and switched in only when complete,
// and conversations keep the current set's voice until then, so the user never hears
// two voices. Recording happens when the messages aren't in the selected voice (first
// setup, or after changing the voice) or when asked from the diagnostics page.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t voicepack_init(void);

/** Voice the device's messages are currently in (a recorded set, or the built-in clips). */
const char *voicepack_active_voice(void);
/** Gemini voice to use for conversations, so they match the messages. */
const char *voicepack_conversation_voice(void);

/** True if the messages aren't in the selected voice yet (or a re-record was requested). */
bool voicepack_needed(void);
/** Record in the background (call when online, idle, with an API key). */
esp_err_t voicepack_start(void);
bool voicepack_busy(void);
/** Stop now (e.g. the user said "Jarvis"); unfinished lines are recorded later. */
void voicepack_abort(void);
/** The selected voice changed: stop a recording in the old one. */
void voicepack_voice_changed(void);
/** Record every line again in the selected voice (diagnostics button). */
esp_err_t voicepack_rerecord(void);

bool voicepack_last_failed(void);
/** Set once when a complete set was just switched in (cleared on read). */
bool voicepack_just_finished(void);
/** Human-readable state for the setup and diagnostics pages. */
void voicepack_status(char *buf, size_t len);
