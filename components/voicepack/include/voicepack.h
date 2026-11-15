// SPDX-License-Identifier: MIT
//
// One voice, everywhere: the device's own lines (wake-word answers, status messages,
// tests) are recorded at runtime by asking Gemini Live to read tools/prompts.tsv in
// the voice used for conversations, and overwrite the built-in clips.
//
// The most-heard lines (wake-word answers, "Ready", "Settings saved") are recorded
// first, in about 15 seconds; then messages and conversations switch to the new voice
// together. The remaining, rarer lines are recorded while the device is idle.
// Recording happens when the messages aren't in the selected voice (first setup, or
// after changing the voice) or when asked from the diagnostics page.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t voicepack_init(void);

/** Voice the device's messages are currently in (a recorded set, or the built-in clips). */
const char *voicepack_active_voice(void);
/** Gemini voice to use for conversations, so they match the messages. */
const char *voicepack_conversation_voice(void);

/** True while any line still needs recording (or a re-record was requested). */
bool voicepack_needed(void);
/** True while the most-heard lines ("Yes?", "Ready...") aren't in the selected voice yet. */
bool voicepack_priority_pending(void);
/**
 * Record in the background (call when online, not in a conversation, with an API key).
 * The most-heard lines go first and switch the voice over as soon as they exist;
 * include_rest also records the remaining lines (do that when the device is idle).
 */
esp_err_t voicepack_start(bool include_rest);
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
