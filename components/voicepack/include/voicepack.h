// SPDX-License-Identifier: MIT
//
// One voice, everywhere: the device's own lines (wake-word answers, status messages,
// tests) are recorded by asking Gemini Live to read tools/prompts.tsv in the voice
// used for conversations, and overwrite the built-in clips.
//
// Recording is a job started when the user saves the setup page with a new voice (or
// while the messages aren't in the selected voice yet), or from the diagnostics page.
// It runs until finished, reporting progress; messages and conversations switch to the
// new voice together when every line is recorded. If the user starts a conversation
// meanwhile, the job pauses and resumes afterwards.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool busy;              // recording right now
    bool pending;           // a job exists (running or paused)
    int done, total;        // lines recorded so far / lines in the job
    char voice[32];         // voice being recorded (or in use when idle)
    char active_voice[32];  // voice messages and conversations use now
    char error[128];        // why the last job stopped, if it failed
    char status[352];       // one-line summary for people
} voicepack_progress_t;

esp_err_t voicepack_init(void);

/** Voice the device's messages are currently in (a recorded set, or the built-in clips). */
const char *voicepack_active_voice(void);
/** Gemini voice to use for conversations, so they match the messages. */
const char *voicepack_conversation_voice(void);
/** True if a complete recorded set in the selected voice is in use. */
bool voicepack_matches_selected(void);

/** Recording only starts when this says the device is free (not in a conversation). */
void voicepack_set_gate(bool (*can_start)(void));
/** Start recording in the selected voice now, or as soon as the device is free. */
esp_err_t voicepack_request(bool redo);
/** Continue a paused job (call when idle and online). */
esp_err_t voicepack_resume(void);
bool voicepack_pending(void);
/** Voice of the current or paused job. */
const char *voicepack_progress_voice(void);
bool voicepack_busy(void);
/** Pause now (e.g. the user said "Jarvis"); voicepack_resume() continues. */
void voicepack_abort(void);

void voicepack_progress(voicepack_progress_t *out);
void voicepack_status(char *buf, size_t len);
/** Set once when a job just finished (cleared on read). */
bool voicepack_just_finished(void);
