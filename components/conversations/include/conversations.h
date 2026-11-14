// SPDX-License-Identifier: MIT
//
// Saved conversations ("remember this conversation" / "let's resume our conversation").
//
// During a session the device keeps a rolling transcript (from the Live API's
// input/output transcription). Saving stores the model's title + summary and the
// transcript tail on flash, so a later session can load it and pick up the thread.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#define CONVERSATIONS_MAX 20             // oldest are deleted beyond this

esp_err_t conversations_init(void);

/** Start a fresh session (clears the transcript buffer and "resumed" link). */
void conversations_session_begin(void);
/** Append transcribed text. role: "user" or "assistant". */
void conversations_add_text(const char *role, const char *text);

/**
 * Save the current session. If this session resumed a saved conversation,
 * that entry is updated (and moved to the top) instead of creating a new one.
 * Returns {"saved":true,"id":N,"title":"..."} or {"error":"..."}.
 */
cJSON *conversations_save(const char *title, const char *summary);
/** {"conversations":[{"number":1,"id":N,"title":"...","saved":"2026-11-14 18:05"}...]} newest first. */
cJSON *conversations_list(void);
/** {"id","title","saved","summary","transcript"} and marks the session as resumed. */
cJSON *conversations_load(int id);
cJSON *conversations_delete(int id);
/** Number of saved conversations (-1 if storage is unavailable). */
int conversations_count(void);
