// SPDX-License-Identifier: MIT
//
// Status lights on the board's RGB LED ring.
//
// States persist until changed. Events are short one-shot animations played on
// top of the current state, which resumes afterwards.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LEDS_OFF,            // idle, waiting for "Jarvis" (dark to save battery)
    LEDS_CONNECTING,     // starting up / joining Wi-Fi / opening the session: blue pulse
    LEDS_LISTENING,      // ready for your voice: rainbow rotating around the ring
    LEDS_THINKING,       // working on a request (tool call, waiting for the reply): purple spinner
    LEDS_SPEAKING,       // assistant is talking: soft white glow
    LEDS_SETUP_NEEDED,   // missing Wi-Fi or API key: one amber blink every 4 s
    LEDS_REMINDER,       // a reminder is due: amber breathing until dismissed
    LEDS_ERROR,          // something failed: 3 red blinks, then off
    LEDS_STATE_COUNT
} leds_state_t;

typedef enum {
    LEDS_EVENT_WAKE,     // wake word heard: quick white flash
    LEDS_EVENT_SAVED,    // something saved (conversation, settings): 2 green blinks
    LEDS_EVENT_DELETED,  // something deleted: orange sweep
    LEDS_EVENT_COUNT
} leds_event_t;

/** Called when the state actually changes (not for previews). Keep it quick. */
typedef void (*leds_change_cb_t)(leds_state_t from, leds_state_t to);

esp_err_t leds_init(void);
void leds_on_change(leds_change_cb_t cb);
void leds_set(leds_state_t state);
leds_state_t leds_get(void);
void leds_event(leds_event_t event);

/** For the diagnostics legend: names, descriptions and a preview. */
const char *leds_state_name(leds_state_t s);
const char *leds_state_description(leds_state_t s);
const char *leds_event_name(leds_event_t e);
const char *leds_event_description(leds_event_t e);
/** Show a state or event for a few seconds, then restore. Name from the functions above. */
bool leds_preview(const char *name);
