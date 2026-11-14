// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*wakeword_cb_t)(void *ctx);

/** Start always-on wake word detection on the mic stream (Wi-Fi may be off). */
esp_err_t wakeword_start(wakeword_cb_t on_detect, void *ctx);
/** Pause detection while a conversation is active. */
void wakeword_pause(bool paused);
