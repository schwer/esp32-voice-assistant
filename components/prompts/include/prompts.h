// SPDX-License-Identifier: MIT
//
// Pre-recorded spoken status messages (work offline). Text lives in tools/prompts.tsv.
#pragma once
#include "esp_err.h"

typedef enum {
    PROMPT_READY,          // "Ready. Say Jarvis when you need me."
    PROMPT_NEED_WIFI,      // "I need Wi-Fi. Use the install page to connect me to your network."
    PROMPT_NEED_API_KEY,   // "I'm online, but I need a Gemini API key. Open my setup page to add one."
    PROMPT_WIFI_LOST,      // "I lost the Wi-Fi connection. I'll keep trying."
    PROMPT_WIFI_BACK,      // "I'm back online."
    PROMPT_NO_INTERNET,    // "Sorry, I can't reach the internet right now."
    PROMPT_GEMINI_FAILED,  // "Sorry, I couldn't reach Gemini. Please check my setup page."
    PROMPT_SAVED,          // "Saved."
    PROMPT_SETTINGS_SAVED, // "Settings saved."
    PROMPT_DELETED,        // "Deleted."
    PROMPT_ERROR,          // "Sorry, something went wrong."
    PROMPT_SPEAKER_TEST,   // "This is a speaker test. If you can hear me, the speaker works."
    PROMPT_WAKE_ACK_1,     // "Yes?"
    PROMPT_WAKE_ACK_2,     // "I'm listening."
    PROMPT_WAKE_ACK_3,     // "How can I help?"
    PROMPT_NOT_READY,      // "I heard you, but I can't hold conversations yet. ..."
    PROMPT_MIC_TEST_START, // "Microphone test. Say hi."
    PROMPT_MIC_TEST_HELLO, // "Hello! I heard you. Here's what you said."
    PROMPT_MIC_TEST_NOTHING, // "I didn't hear anything. Check the microphone levels ..."
    PROMPT_BATTERY_LOW,    // "My battery is at ten percent. Please charge me soon."
    PROMPT_BATTERY_CRITICAL, // "My battery is almost empty. I'll turn off soon unless you charge me."
    PROMPT_TIMER_DONE,     // "Your timer is done."
    PROMPT_ALARM_RINGING,  // "Your alarm is going off."
    PROMPT_COUNT
} prompt_t;

#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>

/** Lines recorded on the device live in PROMPTS_RECORDED_DIR/<Voice>/<id>.ulaw (16 kHz µ-law). */
#define PROMPTS_RECORDED_DIR "/data/voice"

/** Start the prompt player (needs audio_init() to have succeeded). */
esp_err_t prompts_init(void);
/** Queue a status message. Skipped if spoken status messages are turned off. */
void prompts_say(prompt_t p);
/** Queue a message even when status messages are off (e.g. speaker test). */
void prompts_say_always(prompt_t p);
/** The id ("wake_ack_1") and text ("Yes?") of a line. */
const char *prompts_id(prompt_t p);
const char *prompts_text(prompt_t p);
/** The whole embedded phrase list (for detecting when it changes). */
const char *prompts_list_raw(size_t *len);
/** Voice the built-in clips were recorded in (e.g. "Puck"). */
const char *prompts_builtin_voice(void);
/** Play recordings from this directory (a complete set), or NULL for the built-in clips. */
void prompts_set_recorded_dir(const char *dir);
void prompts_recorded_path(const char *dir, prompt_t p, char *buf, size_t len);

/** True if the line will play from a recording in Gemini's voice (else built-in). */
bool prompts_is_recorded(prompt_t p);

/** Answer the wake word with a short, varied acknowledgement ("Yes?", ...). */
void prompts_wake_ack(void);
