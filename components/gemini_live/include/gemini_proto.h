// SPDX-License-Identifier: MIT
//
// Gemini Live API messages: building what we send and reading what the server sends.
// No ESP-IDF dependencies (only cJSON), so it is unit-tested on the host against sample
// server messages (tests/host/test_gemini.c).
//
//   -> {"setup": {model, generationConfig{responseModalities, speechConfig}, systemInstruction,
//                 tools, inputAudioTranscription, outputAudioTranscription,
//                 contextWindowCompression}}
//   <- {"setupComplete": {}}
//   -> {"realtimeInput": {"audio": {"mimeType": "audio/pcm;rate=16000", "data": <base64>}}}
//   -> {"clientContent": {"turns": [{"role": "user", "parts": [{"text": ...}]}], "turnComplete": true}}
//   <- {"serverContent": {"modelTurn": {"parts": [{"inlineData": {"data": <base64 24 kHz PCM>}}]},
//                         "inputTranscription": {"text"}, "outputTranscription": {"text"},
//                         "interrupted": true, "turnComplete": true}}
//   <- {"toolCall": {"functionCalls": [{"id", "name", "args"}]}}
//   -> {"toolResponse": {"functionResponses": [{"id", "name", "response"}]}}
//   <- {"goAway": {...}}
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// ---------------------------------------------------------------- base64

/** Encoded length (without terminator) for n bytes. */
#define GEMINI_B64_LEN(n) (4 * (((n) + 2) / 3))
/** Encode; writes GEMINI_B64_LEN(n) chars + NUL. Returns the length written. */
size_t gemini_b64_encode(const uint8_t *in, size_t n, char *out);
/** Decode standard base64 (padding optional). Returns bytes written, or -1 if invalid or too long. */
long gemini_b64_decode(const char *in, size_t len, uint8_t *out, size_t cap);

// ---------------------------------------------------------------- client messages

typedef struct {
    const char *model;               // without "models/"
    const char *voice;
    const char *system_instruction;
    cJSON *tools;                    // [{functionDeclarations: [...]}] or NULL; consumed
    bool google_search;
    bool transcription;              // ask for input/output transcripts
} gemini_setup_t;

/** The setup message (caller frees). Takes ownership of s->tools and sets it to NULL. */
char *gemini_build_setup(gemini_setup_t *s);
/** A text turn (caller frees). */
char *gemini_build_text_turn(const char *text);

/** Size of the buffer gemini_build_audio needs for `samples` 16-bit samples. */
#define GEMINI_AUDIO_MSG_MAX(samples) (96 + GEMINI_B64_LEN((samples) * 2))
/** A realtimeInput audio message for 16 kHz mono PCM. Returns its length (0 if `cap` is too small). */
size_t gemini_build_audio(const int16_t *pcm, size_t samples, char *out, size_t cap);

/** Tool responses: start a message, add one response per call, print (frees `msg`). */
cJSON *gemini_tool_response_new(void);
void gemini_tool_response_add(cJSON *msg, const char *id, const char *name, cJSON *result /* consumed */);
char *gemini_print_and_free(cJSON *msg);

// ---------------------------------------------------------------- server messages

typedef struct {
    void (*setup_complete)(void *ctx);
    /** Gemini's voice: 24 kHz mono PCM. */
    void (*audio)(const int16_t *pcm, size_t samples, void *ctx);
    void (*input_transcript)(const char *text, void *ctx);
    void (*output_transcript)(const char *text, void *ctx);
    void (*interrupted)(void *ctx);
    void (*turn_complete)(void *ctx);
    /** One call per functionCall; then tool_calls_done once for the toolCall message. */
    void (*tool_call)(const char *id, const char *name, const cJSON *args, void *ctx);
    void (*tool_calls_done)(void *ctx);
    void (*go_away)(void *ctx);
    void *ctx;
} gemini_handlers_t;

/** Reusable decode buffer for audio (grows as needed). */
typedef struct {
    uint8_t *buf;
    size_t cap;
} gemini_scratch_t;

void gemini_scratch_free(gemini_scratch_t *s);

/**
 * Read one server message and call the handlers, in this order: setup_complete,
 * tool_call(s) + tool_calls_done, audio parts, transcripts, interrupted, turn_complete,
 * go_away. Unknown fields are ignored. Returns false if it isn't valid JSON.
 */
bool gemini_parse_message(const char *json, size_t len, const gemini_handlers_t *h, gemini_scratch_t *scratch);

/** WebSocket close frame payload -> status code and reason text. */
int gemini_parse_close(const uint8_t *data, size_t len, char *reason, size_t cap);

// ---------------------------------------------------------------- transcripts

#define GEMINI_TRANSCRIPT_MAX 2048
/** Collects the pieces of one turn's transcripts (they arrive word by word). */
typedef struct {
    char user[GEMINI_TRANSCRIPT_MAX];
    char assistant[GEMINI_TRANSCRIPT_MAX];
} gemini_transcript_t;

void gemini_transcript_reset(gemini_transcript_t *t);
void gemini_transcript_append(char *buf, const char *text);   // truncates at the limit
