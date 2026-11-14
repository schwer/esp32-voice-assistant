// SPDX-License-Identifier: MIT
//
// Records the device's spoken lines (wake word answers, status messages) in the
// conversation voice, by asking Gemini Live to read each line from tools/prompts.tsv
// with the user's model and voice. Recordings live on flash, so they work offline.
// Re-recorded automatically when the voice, model or phrase list changes.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Check existing recordings; use them if they match the current voice. */
esp_err_t voicepack_init(void);
/** True if recordings are missing or were made with another voice/model/phrase list. */
bool voicepack_needed(void);
/** Start recording in the background (call when online, idle, with an API key). */
esp_err_t voicepack_start(void);
bool voicepack_busy(void);
/** Stop recording now (e.g. the user said "Jarvis"); it resumes later. */
void voicepack_abort(void);
/** Human-readable state, e.g. "recorded in Puck" or "recording 7 of 21". */
void voicepack_status(char *buf, size_t len);
/** True if the last recording attempt failed (not interrupted). */
bool voicepack_last_failed(void);
/** Set when a full recording just finished (cleared on read). */
bool voicepack_just_finished(void);
