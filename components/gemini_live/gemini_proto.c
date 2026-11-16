// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gemini_proto.h"

// ---------------------------------------------------------------- base64

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t gemini_b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;      // also accept the URL-safe alphabet
    if (c == '/' || c == '_') return 63;
    return -1;
}

long gemini_b64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    while (len && in[len - 1] == '=') len--;
    if (len % 4 == 1) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_value(in[i]);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)o;
}

// ---------------------------------------------------------------- client messages

char *gemini_build_setup(gemini_setup_t *s)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    char model[128];
    snprintf(model, sizeof(model), "models/%s", s->model ? s->model : "");
    cJSON_AddStringToObject(setup, "model", model);

    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(gen, "responseModalities"), cJSON_CreateString("AUDIO"));
    cJSON *voice = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(gen, "speechConfig"), "voiceConfig"),
        "prebuiltVoiceConfig");
    cJSON_AddStringToObject(voice, "voiceName", s->voice ? s->voice : "");

    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", s->system_instruction ? s->system_instruction : "");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(cJSON_AddObjectToObject(setup, "systemInstruction"), "parts"), part);

    cJSON *tools = s->tools ? s->tools : cJSON_CreateArray();
    s->tools = NULL;
    if (s->google_search) {
        cJSON *gs = cJSON_CreateObject();
        cJSON_AddObjectToObject(gs, "googleSearch");
        cJSON_AddItemToArray(tools, gs);
    }
    if (cJSON_GetArraySize(tools) > 0) {
        cJSON_AddItemToObject(setup, "tools", tools);
    } else {
        cJSON_Delete(tools);                         // the API rejects an empty tools list
    }
    if (s->transcription) {
        cJSON_AddObjectToObject(setup, "inputAudioTranscription");
        cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    }
    if (s->proactive_audio) {
        // Native-audio models can then ignore background talk, TV and people not
        // addressing the assistant, and only answer when spoken to.
        cJSON_AddBoolToObject(cJSON_AddObjectToObject(setup, "proactivity"), "proactiveAudio", true);
    }
    // Keep long conversations affordable: older turns slide out of the context.
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(setup, "contextWindowCompression"), "slidingWindow");
    return gemini_print_and_free(root);
}

char *gemini_build_text_turn(const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON *cc = cJSON_AddObjectToObject(msg, "clientContent");
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "role", "user");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(turn, "parts"), part);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(cc, "turns"), turn);
    cJSON_AddBoolToObject(cc, "turnComplete", true);
    return gemini_print_and_free(msg);
}

size_t gemini_build_audio(const int16_t *pcm, size_t samples, char *out, size_t cap)
{
    static const char head[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    static const char tail[] = "\"}}}";
    size_t hl = sizeof(head) - 1, tl = sizeof(tail) - 1;
    size_t bytes = samples * sizeof(int16_t);
    if (cap < hl + GEMINI_B64_LEN(bytes) + tl + 1) return 0;
    memcpy(out, head, hl);
    size_t b = gemini_b64_encode((const uint8_t *)pcm, bytes, out + hl);   // little-endian PCM
    memcpy(out + hl + b, tail, tl + 1);
    return hl + b + tl;
}

cJSON *gemini_tool_response_new(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddArrayToObject(cJSON_AddObjectToObject(msg, "toolResponse"), "functionResponses");
    return msg;
}

void gemini_tool_response_add(cJSON *msg, const char *id, const char *name, cJSON *result)
{
    if (!result) {
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "error", "no handler");
    }
    cJSON *r = cJSON_CreateObject();
    if (id) cJSON_AddStringToObject(r, "id", id);
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddItemToObject(r, "response", result);
    cJSON_AddItemToArray(cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "toolResponse"), "functionResponses"), r);
}

char *gemini_print_and_free(cJSON *msg)
{
    char *json = msg ? cJSON_PrintUnformatted(msg) : NULL;
    cJSON_Delete(msg);
    return json;
}

// ---------------------------------------------------------------- server messages

void gemini_scratch_free(gemini_scratch_t *s)
{
    free(s->buf);
    s->buf = NULL;
    s->cap = 0;
}

static void audio_part(const char *b64, const gemini_handlers_t *h, gemini_scratch_t *sc)
{
    size_t len = strlen(b64), need = len / 4 * 3 + 3;
    if (need > sc->cap) {
        uint8_t *nb = realloc(sc->buf, need);
        if (!nb) return;
        sc->buf = nb;
        sc->cap = need;
    }
    long n = gemini_b64_decode(b64, len, sc->buf, sc->cap);
    if (n >= 2 && h->audio) h->audio((const int16_t *)sc->buf, (size_t)n / 2, h->ctx);
}

static const char *text_of(const cJSON *parent, const char *field)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(parent, field), "text"));
}

bool gemini_parse_message(const char *json, size_t len, const gemini_handlers_t *h, gemini_scratch_t *scratch)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_GetObjectItem(root, "setupComplete") && h->setup_complete) h->setup_complete(h->ctx);

    const cJSON *tc = cJSON_GetObjectItem(root, "toolCall");
    if (tc) {
        const cJSON *fc;
        cJSON_ArrayForEach(fc, cJSON_GetObjectItem(tc, "functionCalls")) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "name"));
            if (name && h->tool_call) {
                h->tool_call(cJSON_GetStringValue(cJSON_GetObjectItem(fc, "id")), name,
                             cJSON_GetObjectItem(fc, "args"), h->ctx);
            }
        }
        if (h->tool_calls_done) h->tool_calls_done(h->ctx);
    }

    const cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
    if (sc) {
        const cJSON *part;
        cJSON_ArrayForEach(part, cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "modelTurn"), "parts")) {
            const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(part, "inlineData"), "data"));
            if (b64) audio_part(b64, h, scratch);
        }
        const char *t = text_of(sc, "inputTranscription");
        if (t && h->input_transcript) h->input_transcript(t, h->ctx);
        t = text_of(sc, "outputTranscription");
        if (t && h->output_transcript) h->output_transcript(t, h->ctx);
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "interrupted")) && h->interrupted) h->interrupted(h->ctx);
        if (cJSON_IsTrue(cJSON_GetObjectItem(sc, "turnComplete")) && h->turn_complete) h->turn_complete(h->ctx);
    }
    if (cJSON_GetObjectItem(root, "goAway") && h->go_away) h->go_away(h->ctx);
    cJSON_Delete(root);
    return true;
}

int gemini_parse_close(const uint8_t *d, size_t len, char *reason, size_t cap)
{
    int code = len >= 2 ? (d[0] << 8) | d[1] : 0;
    size_t n = len > 2 ? len - 2 : 0;
    if (cap) {
        if (n > cap - 1) n = cap - 1;
        if (n) memcpy(reason, d + 2, n);
        reason[n] = 0;
    }
    return code;
}

// ---------------------------------------------------------------- transcripts

void gemini_transcript_reset(gemini_transcript_t *t)
{
    t->user[0] = t->assistant[0] = 0;
}

void gemini_transcript_append(char *buf, const char *text)
{
    size_t n = strlen(buf);
    if (n >= GEMINI_TRANSCRIPT_MAX - 1) return;
    size_t room = GEMINI_TRANSCRIPT_MAX - 1 - n, l = strlen(text);
    if (l > room) l = room;
    memcpy(buf + n, text, l);
    buf[n + l] = 0;
}

void gemini_transcript_keep_tail(char *buf, size_t keep)
{
    size_t n = strlen(buf);
    if (n > keep) memmove(buf, buf + n - keep, keep + 1);
}

/** Next lowercase word (letters/digits/apostrophes) from *p into w; false at the end. */
static bool next_word(const char **p, char *w, size_t cap)
{
    const char *s = *p;
    while (*s && !isalnum((unsigned char)*s)) s++;
    if (!*s) { *p = s; return false; }
    size_t n = 0;
    while (*s && (isalnum((unsigned char)*s) || *s == '\'')) {
        if (n + 1 < cap) w[n++] = (char)tolower((unsigned char)*s);
        s++;
    }
    w[n] = 0;
    *p = s;
    return true;
}

static bool contains_word(const char *text, const char *word)
{
    char w[48];
    const char *p = text;
    while (next_word(&p, w, sizeof(w))) {
        if (strcmp(w, word) == 0) return true;
    }
    return false;
}

bool gemini_looks_like_echo(const char *heard, const char *said)
{
    if (!heard || !said || !said[0]) return false;
    char w[48];
    const char *p = heard;
    int words = 0, found = 0;
    while (next_word(&p, w, sizeof(w))) {
        words++;
        if (contains_word(said, w)) found++;
    }
    return words >= 3 && found * 10 >= words * 7;     // 70% of the heard words were just said
}
