// SPDX-License-Identifier: MIT
//
// Records the device's spoken lines (wake word answers, status messages) in the
// conversation voice, by asking Gemini Live to read each line from tools/prompts.tsv
// with the user's model and voice. Recordings live on flash, so they work offline.
// Recording happens only when the user picks a different voice on the setup page.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Check existing recordings; use them if they match the current voice. */
esp_err_t voicepack_init(void);
/** The user picked a different voice: record the lines in it when online and idle. */
void voicepack_voice_changed(void);
/** True while a recording requested by a voice change is still outstanding. */
bool voicepack_needed(void);
/** Start recording in the background (call when online, idle, with an API key). */
esp_err_t voicepack_start(void);
bool voicepack_busy(void);
/** Stop recording now (e.g. the user said "Jarvis"); it resumes later. */
void voicepack_abort(void);
/** Delete the recordings and go back to the built-in clips (stops any recording). */
esp_err_t voicepack_reset(void);
/** Human-readable state, e.g. "recorded in Puck" or "recording 7 of 21". */
void voicepack_status(char *buf, size_t len);
/** True if the last recording attempt failed (not interrupted). */
bool voicepack_last_failed(void);
/** Set when a full recording just finished (cleared on read). */
bool voicepack_just_finished(void);
