// SPDX-License-Identifier: MIT
// Gemini Live messages: what the firmware sends, and replaying server conversations
// (fixtures/gemini/*.jsonl) through the parser the firmware uses.
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "gemini_proto.h"
#include "test.h"

// ---------------------------------------------------------------- helpers

/** Records parser events as text, e.g. "setup|in:What's the|tool:fc-1:get_weather:{"days":2}|". */
typedef struct {
    char log[8192];
    int16_t audio[256];
    size_t audio_n;
    gemini_transcript_t tr;
    char turns[2][4][256];           // completed transcripts per turn: [user/assistant][turn]
    int n_turns;
} rec_t;

static void add(rec_t *r, const char *s) { strncat(r->log, s, sizeof(r->log) - strlen(r->log) - 1); }

static void r_setup(void *c) { add(c, "setup|"); }
static void r_audio(const int16_t *pcm, size_t n, void *c)
{
    rec_t *r = c;
    char b[32];
    snprintf(b, sizeof(b), "audio:%zu|", n);
    add(r, b);
    for (size_t i = 0; i < n && r->audio_n < 256; i++) r->audio[r->audio_n++] = pcm[i];
}
static void r_in(const char *t, void *c) { rec_t *r = c; add(r, "in:"); add(r, t); add(r, "|"); gemini_transcript_append(r->tr.user, t); }
static void r_out(const char *t, void *c) { rec_t *r = c; add(r, "out:"); add(r, t); add(r, "|"); gemini_transcript_append(r->tr.assistant, t); }
static void r_interrupted(void *c) { add(c, "interrupted|"); }
static void r_turn(void *c)
{
    rec_t *r = c;
    add(r, "turn|");
    if (r->n_turns < 4) {
        strncpy(r->turns[0][r->n_turns], r->tr.user, 255);
        strncpy(r->turns[1][r->n_turns], r->tr.assistant, 255);
        r->n_turns++;
    }
    gemini_transcript_reset(&r->tr);
}
static void r_tool(const char *id, const char *name, const cJSON *args, void *c)
{
    char *a = cJSON_PrintUnformatted(args);
    char b[256];
    snprintf(b, sizeof(b), "tool:%s:%s:%s|", id ? id : "-", name, a ? a : "null");
    add(c, b);
    free(a);
}
static void r_tools_done(void *c) { add(c, "tools-done|"); }
static void r_goaway(void *c) { add(c, "goaway|"); }

static gemini_handlers_t handlers(rec_t *r)
{
    gemini_handlers_t h = { r_setup, r_audio, r_in, r_out, r_interrupted, r_turn, r_tool, r_tools_done, r_goaway, r };
    return h;
}

static int replay(const char *file, rec_t *r)
{
    char path[256];
    snprintf(path, sizeof(path), "fixtures/gemini/%s", file);
    FILE *f = fopen(path, "r");
    if (!f) { printf("  cannot open %s\n", path); return -1; }
    gemini_handlers_t h = handlers(r);
    gemini_scratch_t scratch = { 0 };
    char line[16384];
    int bad = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!gemini_parse_message(line, strlen(line), &h, &scratch)) bad++;
    }
    fclose(f);
    gemini_scratch_free(&scratch);
    return bad;
}

static cJSON *parse(const char *json) { return cJSON_Parse(json); }
static const cJSON *at(const cJSON *o, const char *path)
{
    char buf[128];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *k = strtok(buf, "."); k && o; k = strtok(NULL, ".")) {
        o = (*k >= '0' && *k <= '9') ? cJSON_GetArrayItem(o, atoi(k)) : cJSON_GetObjectItem(o, k);
    }
    return o;
}
static const char *str_at(const cJSON *o, const char *path) { return cJSON_GetStringValue(at(o, path)); }

// ---------------------------------------------------------------- base64

static void base64_round_trips_all_lengths(void)
{
    uint8_t in[64], back[64];
    char enc[GEMINI_B64_LEN(64) + 1];
    for (int i = 0; i < 64; i++) in[i] = (uint8_t)(i * 37 + 11);
    for (size_t n = 0; n <= 64; n++) {
        size_t e = gemini_b64_encode(in, n, enc);
        CHECK(e == GEMINI_B64_LEN(n) && strlen(enc) == e);
        long d = gemini_b64_decode(enc, e, back, sizeof(back));
        CHECK(d == (long)n && memcmp(in, back, n) == 0);
    }
    CHECK(strcmp((gemini_b64_encode((const uint8_t *)"Man", 3, enc), enc), "TWFu") == 0);
    CHECK(strcmp((gemini_b64_encode((const uint8_t *)"Ma", 2, enc), enc), "TWE=") == 0);
}

static void base64_rejects_garbage_and_overflow(void)
{
    uint8_t out[8];
    CHECK(gemini_b64_decode("TW!u", 4, out, sizeof(out)) == -1);
    CHECK(gemini_b64_decode("T", 1, out, sizeof(out)) == -1);
    CHECK(gemini_b64_decode("TWFuTWFuTWFu", 12, out, 4) == -1);      // won't overflow the buffer
    CHECK(gemini_b64_decode("TWE", 3, out, sizeof(out)) == 2);         // padding optional
    CHECK(gemini_b64_decode("-_8", 3, out, sizeof(out)) == 2);         // URL-safe alphabet
}

// ---------------------------------------------------------------- what we send

static void setup_message_matches_the_live_api(void)
{
    cJSON *tools = parse("[{\"functionDeclarations\":[{\"name\":\"get_weather\"}]}]");
    gemini_setup_t s = { .model = "gemini-3.8-live", .voice = "Kore", .system_instruction = "Be brief.",
                         .tools = tools, .google_search = true, .transcription = true };
    char *json = gemini_build_setup(&s);
    CHECK(s.tools == NULL);                                         // ownership taken
    cJSON *m = parse(json);
    CHECK(m != NULL);
    CHECK(strcmp(str_at(m, "setup.model"), "models/gemini-3.8-live") == 0);
    CHECK(strcmp(str_at(m, "setup.generationConfig.responseModalities.0"), "AUDIO") == 0);
    CHECK(strcmp(str_at(m, "setup.generationConfig.speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName"), "Kore") == 0);
    CHECK(strcmp(str_at(m, "setup.systemInstruction.parts.0.text"), "Be brief.") == 0);
    CHECK(strcmp(str_at(m, "setup.tools.0.functionDeclarations.0.name"), "get_weather") == 0);
    CHECK(cJSON_IsObject(at(m, "setup.tools.1.googleSearch")));
    CHECK(cJSON_IsObject(at(m, "setup.inputAudioTranscription")));
    CHECK(cJSON_IsObject(at(m, "setup.outputAudioTranscription")));
    CHECK(cJSON_IsObject(at(m, "setup.contextWindowCompression.slidingWindow")));
    cJSON_Delete(m);
    free(json);
}

static void setup_sends_the_language_code_only_when_set(void)
{
    gemini_setup_t s = { .model = "m", .voice = "Charon", .system_instruction = "x", .language_code = "en-GB" };
    char *json = gemini_build_setup(&s);
    cJSON *m = parse(json);
    CHECK(strcmp(str_at(m, "setup.generationConfig.speechConfig.languageCode"), "en-GB") == 0);
    CHECK(!strcmp(str_at(m, "setup.generationConfig.speechConfig.voiceConfig.prebuiltVoiceConfig.voiceName"), "Charon"));
    cJSON_Delete(m);
    free(json);
    for (int i = 0; i < 2; i++) {                    // NULL and "" both mean the model's default
        gemini_setup_t d = { .model = "m", .voice = "Puck", .system_instruction = "x", .language_code = i ? "" : NULL };
        json = gemini_build_setup(&d);
        m = parse(json);
        CHECK(at(m, "setup.generationConfig.speechConfig.languageCode") == NULL);
        cJSON_Delete(m);
        free(json);
    }
}

static void setup_without_tools_or_transcripts_omits_them(void)
{
    // The voice recorder: no tools (the API rejects an empty list) and no transcripts.
    gemini_setup_t s = { .model = "m", .voice = "Puck", .system_instruction = "Read lines." };
    char *json = gemini_build_setup(&s);
    cJSON *m = parse(json);
    CHECK(at(m, "setup.tools") == NULL);
    CHECK(at(m, "setup.inputAudioTranscription") == NULL);
    cJSON_Delete(m);
    free(json);
}

static void proactive_audio_is_requested_only_when_enabled(void)
{
    gemini_setup_t on = { .model = "m", .voice = "v", .proactive_audio = true };
    char *json = gemini_build_setup(&on);
    cJSON *m = parse(json);
    CHECK(cJSON_IsTrue(at(m, "setup.proactivity.proactiveAudio")));
    cJSON_Delete(m);
    free(json);
    gemini_setup_t off = { .model = "m", .voice = "v" };
    json = gemini_build_setup(&off);
    m = parse(json);
    CHECK(at(m, "setup.proactivity") == NULL);
    cJSON_Delete(m);
    free(json);
}

static void account_errors_are_recognised(void)
{
    CHECK(gemini_error_is_account("Your prepayment credits are depleted. Please go to AI Studio at "
                                  "https://ai.studio/project (code 1011)"));
    CHECK(gemini_error_is_account("You exceeded your current quota (code 1011)"));
    CHECK(gemini_error_is_account("RESOURCE_EXHAUSTED"));
    CHECK(gemini_error_is_account("API key not valid. Please pass a valid API key. (code 1007)"));
    CHECK(gemini_error_is_account("Billing account not active"));
    CHECK(!gemini_error_is_account("Unknown name \"proactivity\" at 'setup': Cannot find field. (code 1007)"));
    CHECK(!gemini_error_is_account("network error talking to Gemini"));
    CHECK(!gemini_error_is_account(""));
    CHECK(!gemini_error_is_account(NULL));
}

static void echo_detection(void)
{
    const char *said = "Tomorrow will be sunny and mild, with a high of 64 degrees.";
    // Gemini's own words coming back through the mics (transcribed slightly differently).
    CHECK(gemini_looks_like_echo("sunny and mild with a high", said));
    CHECK(gemini_looks_like_echo("Tomorrow will be SUNNY", said));
    // The user really interrupting.
    CHECK(!gemini_looks_like_echo("stop, what about Saturday?", said));
    CHECK(!gemini_looks_like_echo("no wait I meant Paris", said));
    // Too little to judge, or nothing said yet.
    CHECK(!gemini_looks_like_echo("sunny", said));
    CHECK(!gemini_looks_like_echo("will be sunny and mild", ""));
    CHECK(!gemini_looks_like_echo(NULL, said));
    // Rolling buffer keeps only the most recent words.
    char buf[GEMINI_TRANSCRIPT_MAX] = "old words here. ";
    gemini_transcript_append(buf, "newest part");
    gemini_transcript_keep_tail(buf, 11);
    CHECK(strcmp(buf, "newest part") == 0);
}

static void audio_message_carries_16khz_pcm(void)
{
    int16_t pcm[1600];
    for (int i = 0; i < 1600; i++) pcm[i] = (int16_t)(i * 41 - 30000);
    static char out[GEMINI_AUDIO_MSG_MAX(1600)];
    size_t n = gemini_build_audio(pcm, 1600, out, sizeof(out));
    CHECK(n > 0 && n == strlen(out));
    cJSON *m = parse(out);
    CHECK(strcmp(str_at(m, "realtimeInput.audio.mimeType"), "audio/pcm;rate=16000") == 0);
    const char *b64 = str_at(m, "realtimeInput.audio.data");
    int16_t back[1600];
    CHECK(gemini_b64_decode(b64, strlen(b64), (uint8_t *)back, sizeof(back)) == (long)sizeof(back));
    CHECK(memcmp(pcm, back, sizeof(back)) == 0);
    cJSON_Delete(m);
    CHECK(gemini_build_audio(pcm, 1600, out, 100) == 0);              // too small: nothing written
}

static void text_turn_and_tool_response_shapes(void)
{
    char *t = gemini_build_text_turn("Read this line aloud exactly: \"Yes?\"");
    cJSON *m = parse(t);
    CHECK(strcmp(str_at(m, "clientContent.turns.0.role"), "user") == 0);
    CHECK(strcmp(str_at(m, "clientContent.turns.0.parts.0.text"), "Read this line aloud exactly: \"Yes?\"") == 0);
    CHECK(cJSON_IsTrue(at(m, "clientContent.turnComplete")));
    cJSON_Delete(m);
    free(t);

    cJSON *msg = gemini_tool_response_new();
    gemini_tool_response_add(msg, "fc-1", "get_weather", parse("{\"now\":{\"temperature\":51}}"));
    gemini_tool_response_add(msg, NULL, "end_conversation", NULL);   // no handler -> error object
    char *r = gemini_print_and_free(msg);
    m = parse(r);
    CHECK(strcmp(str_at(m, "toolResponse.functionResponses.0.id"), "fc-1") == 0);
    CHECK(strcmp(str_at(m, "toolResponse.functionResponses.0.name"), "get_weather") == 0);
    CHECK(cJSON_GetNumberValue(at(m, "toolResponse.functionResponses.0.response.now.temperature")) == 51);
    CHECK(at(m, "toolResponse.functionResponses.1.id") == NULL);
    CHECK(strcmp(str_at(m, "toolResponse.functionResponses.1.response.error"), "no handler") == 0);
    cJSON_Delete(m);
    free(r);
}

// ---------------------------------------------------------------- replaying server messages

static void replay_weather_conversation(void)
{
    rec_t *r = calloc(1, sizeof(rec_t));
    CHECK(replay("conversation.jsonl", r) == 0);
    CHECK(strcmp(r->log,
        "setup|in:What's the|in: weather tomorrow?|"
        "tool:fc-1:get_weather:{\"days\":2}|tools-done|"
        "audio:8|out:Tomorrow will be|audio:4|out: sunny and mild.|turn|goaway|") == 0);
    if (t_failures) printf("    log was: %s\n", r->log);
    // Audio decoded sample-exact, little-endian, across messages.
    const int16_t expect[] = { 0, 1000, -1000, 32767, -32768, 12345, -12345, 1, 5, -5, 300, -300 };
    CHECK(r->audio_n == 12 && memcmp(r->audio, expect, sizeof(expect)) == 0);
    // Transcript pieces are joined into whole turns (used by "remember this conversation").
    CHECK(r->n_turns == 1);
    CHECK(strcmp(r->turns[0][0], "What's the weather tomorrow?") == 0);
    CHECK(strcmp(r->turns[1][0], "Tomorrow will be sunny and mild.") == 0);
    free(r);
}

static void replay_interruption(void)
{
    rec_t *r = calloc(1, sizeof(rec_t));
    CHECK(replay("interruption.jsonl", r) == 0);
    CHECK(strcmp(r->log, "setup|audio:8|out:Here is a long|in:Stop.|interrupted|turn|") == 0);
    CHECK(strcmp(r->turns[0][0], "Stop.") == 0);
    free(r);
}

static void replay_two_tool_calls_and_multi_part_audio(void)
{
    rec_t *r = calloc(1, sizeof(rec_t));
    CHECK(replay("two_tools.jsonl", r) == 0);
    CHECK(strcmp(r->log,
        "setup|tool:fc-7:get_battery_status:{}|tool:fc-8:get_weather:{\"location\":\"Paris, France\"}|tools-done|"
        "audio:4|audio:8|out:Battery is fine; Paris is rainy.|turn|") == 0);
    if (t_failures) printf("    log was: %s\n", r->log);
    free(r);
}

static void odd_and_broken_messages_are_handled(void)
{
    rec_t *r = calloc(1, sizeof(rec_t));
    gemini_handlers_t h = handlers(r);
    gemini_scratch_t sc = { 0 };
    CHECK(!gemini_parse_message("not json", 8, &h, &sc));
    CHECK(!gemini_parse_message("[1,2]", 5, &h, &sc));                         // not an object
    CHECK(!gemini_parse_message("{\"setupComplete\":", 17, &h, &sc));           // truncated
    CHECK(gemini_parse_message("{}", 2, &h, &sc));
    CHECK(gemini_parse_message("{\"toolCallCancellation\":{\"ids\":[\"x\"]}}", 39, &h, &sc));
    const char *weird =
        "{\"serverContent\":{\"modelTurn\":{\"parts\":["
        "{\"text\":\"thinking...\"},"                                   // text part: ignored
        "{\"inlineData\":{\"data\":\"!!!not base64\"}},"                // bad audio: skipped
        "{\"inlineData\":{}},"                                          // no data
        "{\"inlineData\":{\"data\":\"AA\"}}"                            // 1 byte: not a sample
        "]},\"interrupted\":false,\"turnComplete\":false}}";
    CHECK(gemini_parse_message(weird, strlen(weird), &h, &sc));
    const char *nameless = "{\"toolCall\":{\"functionCalls\":[{\"id\":\"1\"}]}}";
    CHECK(gemini_parse_message(nameless, strlen(nameless), &h, &sc));
    CHECK(strcmp(r->log, "tools-done|") == 0);                    // nothing else fired
    // Optional handlers may be missing.
    gemini_handlers_t none = { 0 };
    CHECK(gemini_parse_message("{\"setupComplete\":{},\"goAway\":{}}", 32, &none, &sc));
    gemini_scratch_free(&sc);
    free(r);
}

static void large_audio_chunk_grows_the_buffer(void)
{
    // 1 s of 24 kHz audio in one message (~64 KB of base64).
    size_t n = 24000;
    int16_t *pcm = malloc(n * 2);
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)(i * 7);
    char *b64 = malloc(GEMINI_B64_LEN(n * 2) + 1);
    gemini_b64_encode((const uint8_t *)pcm, n * 2, b64);
    char *json = malloc(strlen(b64) + 128);
    sprintf(json, "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"data\":\"%s\"}}]}}}", b64);
    rec_t *r = calloc(1, sizeof(rec_t));
    gemini_handlers_t h = handlers(r);
    gemini_scratch_t sc = { 0 };
    CHECK(gemini_parse_message(json, strlen(json), &h, &sc));
    CHECK(strcmp(r->log, "audio:24000|") == 0);
    CHECK(sc.cap >= n * 2);
    gemini_scratch_free(&sc);
    free(pcm); free(b64); free(json); free(r);
}

static void close_frame_reason(void)
{
    char reason[32];
    const uint8_t frame[] = { 0x03, 0xF0, 'A', 'P', 'I', ' ', 'k', 'e', 'y', ' ', 'n', 'o', 't', ' ',
                              'v', 'a', 'l', 'i', 'd' };
    CHECK(gemini_parse_close(frame, sizeof(frame), reason, sizeof(reason)) == 1008);
    CHECK(strcmp(reason, "API key not valid") == 0);
    char small[8];
    gemini_parse_close(frame, sizeof(frame), small, sizeof(small));
    CHECK(strcmp(small, "API key") == 0);                           // truncated safely
    CHECK(gemini_parse_close(frame, 1, reason, sizeof(reason)) == 0 && reason[0] == 0);
}

static void transcripts_truncate_safely(void)
{
    gemini_transcript_t t;
    gemini_transcript_reset(&t);
    char chunk[301];
    memset(chunk, 'w', 300);
    chunk[300] = 0;
    for (int i = 0; i < 20; i++) gemini_transcript_append(t.user, chunk);   // 6000 chars offered
    CHECK(strlen(t.user) == GEMINI_TRANSCRIPT_MAX - 1);
}

int main(void)
{
    RUN(base64_round_trips_all_lengths);
    RUN(setup_sends_the_language_code_only_when_set);
    RUN(account_errors_are_recognised);
    RUN(base64_rejects_garbage_and_overflow);
    RUN(setup_message_matches_the_live_api);
    RUN(setup_without_tools_or_transcripts_omits_them);
    RUN(proactive_audio_is_requested_only_when_enabled);
    RUN(echo_detection);
    RUN(audio_message_carries_16khz_pcm);
    RUN(text_turn_and_tool_response_shapes);
    RUN(replay_weather_conversation);
    RUN(replay_interruption);
    RUN(replay_two_tool_calls_and_multi_part_audio);
    RUN(odd_and_broken_messages_are_handled);
    RUN(large_audio_chunk_grows_the_buffer);
    RUN(close_frame_reason);
    RUN(transcripts_truncate_safely);
    TEST_MAIN_END("gemini");
}
