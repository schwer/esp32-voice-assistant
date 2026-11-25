// SPDX-License-Identifier: MIT
//
// Gemini Live API client: one voice conversation over a TLS WebSocket.
//   up:   16 kHz mono PCM from the microphones (after echo cancellation)
//   down: 24 kHz mono PCM from Gemini, resampled and played on the speaker
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum {
    GEMINI_STATE_CONNECTING,
    GEMINI_STATE_READY,          // listening for the user
    GEMINI_STATE_MODEL_SPEAKING, // Gemini's reply is playing
    GEMINI_STATE_CLOSED,         // conversation over (idle timeout, goodbye, server closed)
    GEMINI_STATE_ERROR,          // couldn't connect / rejected (see gemini_live_last_error)
} gemini_state_t;

typedef void (*gemini_state_cb_t)(gemini_state_t state, void *ctx);
/** Called for each functionCall; return the response object (ownership passes to the client). */
typedef cJSON *(*gemini_tool_handler_t)(const char *name, const cJSON *args);
/** Complete transcript of each turn. role: "user" or "assistant". */
typedef void (*gemini_transcript_cb_t)(const char *role, const char *text, void *ctx);

typedef struct {
    const char *api_key;
    const char *model;                   // e.g. "gemini-3.8-live" (no "models/" prefix)
    const char *voice;                   // prebuilt voice, e.g. "Puck"
    const char *system_instruction;      // copied
    cJSON *tools;                        // [{ "functionDeclarations": [...] }]; client takes ownership
    gemini_tool_handler_t tool_handler;
    gemini_state_cb_t on_state;
    gemini_transcript_cb_t on_transcript;
    void *ctx;
    int idle_timeout_s;                  // end after this much silence from both sides
    bool barge_in;                       // let the user interrupt: keep the mic open while Gemini talks
    bool proactive_audio;                // only answer speech meant for the assistant
    bool google_search;                  // add Google Search grounding next to our function tools
    /** Accent region of the voice, e.g. "en-GB" (speechConfig.languageCode). Dropped
     *  automatically once a model has been found to reject it. NULL/"" = default. */
    const char *language_code;
    bool keep_buffered_audio;            // retry: resend speech buffered by the previous attempt
    /** If set, Gemini's audio (24 kHz mono) goes here instead of the speaker. */
    void (*on_audio_out)(const int16_t *pcm, size_t samples, void *ctx);
    /** Called when Gemini finishes a turn (after turnComplete). */
    void (*on_turn_complete)(void *ctx);
} gemini_live_config_t;

/** Does the current model take the language code? -1 not known yet, 0 no, 1 yes. */
int gemini_live_language_ok(void);
/** Record that a session failed only because of the language code (or worked with it). */
void gemini_live_set_language_ok(bool ok);
/** Forget what we learned (the model or the accent region changed). */
void gemini_live_language_unknown(void);
/** The language code the last session actually sent ("" if none). */
const char *gemini_live_language_sent(void);

/** Start connecting. Progress and the end of the session arrive via on_state. */
esp_err_t gemini_live_start(const gemini_live_config_t *cfg);
/** Close the session (safe to call more than once). */
esp_err_t gemini_live_stop(void);
/** Stream microphone audio (buffered until the session is ready). speech = local VAD. */
esp_err_t gemini_live_send_audio(const int16_t *pcm, size_t samples, bool speech);
bool gemini_live_ready(void);
/** Send a text turn (e.g. a line to read aloud). */
esp_err_t gemini_live_send_text(const char *text);
/**
 * True if, during this session, Gemini heard its own voice through the microphones
 * (echo cancellation not working); interruptions were then switched off for the session.
 */
bool gemini_live_echo_detected(void);
/** Human-readable reason for the last error, e.g. "API key not valid". */
const char *gemini_live_last_error(void);
