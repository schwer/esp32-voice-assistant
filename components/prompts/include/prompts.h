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
    PROMPT_COUNT
} prompt_t;

/** Start the prompt player (needs audio_init() to have succeeded). */
esp_err_t prompts_init(void);
/** Queue a status message. Skipped if spoken status messages are turned off. */
void prompts_say(prompt_t p);
/** Queue a message even when status messages are off (e.g. speaker test). */
void prompts_say_always(prompt_t p);
/** Answer the wake word with a short, varied acknowledgement ("Yes?", ...). */
void prompts_wake_ack(void);
