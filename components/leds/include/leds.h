// SPDX-License-Identifier: MIT
// Status animations on the board's RGB LED ring.
#pragma once
#include "esp_err.h"

typedef enum {
    LEDS_OFF,          // idle, waiting for the wake word (dark to save battery)
    LEDS_CONNECTING,   // wake word heard, joining Wi-Fi / opening the session: blue pulse
    LEDS_LISTENING,    // ready for your voice: color cycling around the ring
    LEDS_SPEAKING,     // assistant is talking: soft white glow
    LEDS_ERROR,        // something failed: red blinks, then off
} leds_state_t;

esp_err_t leds_init(void);
void leds_set(leds_state_t state);
/** Diagnostics: color cycle for ~3 s, then off. */
void leds_test(void);
