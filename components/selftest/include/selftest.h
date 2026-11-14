// SPDX-License-Identifier: MIT
// Hardware self-tests started from the diagnostics page.
#pragma once
#include "esp_err.h"

typedef enum {
    SELFTEST_IDLE,
    SELFTEST_RUNNING,
    SELFTEST_HEARD,      // speech captured and played back
    SELFTEST_NOTHING,    // no speech detected
} selftest_state_t;

/**
 * Microphone test: says "Microphone test. Say hi.", listens, answers "Hello!"
 * and plays back what it heard. Runs in the background.
 * ESP_ERR_NOT_SUPPORTED: microphones not running. ESP_ERR_INVALID_STATE: busy.
 */
esp_err_t selftest_mic_start(void);
selftest_state_t selftest_mic_state(float *seconds_heard);
