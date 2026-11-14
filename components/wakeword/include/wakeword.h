// SPDX-License-Identifier: MIT
//
// Voice front end built on Espressif ESP-SR AFE:
//   raw mic + playback reference -> AEC -> WakeNet ("Jarvis") -> VAD -> clean 16 kHz mono
//
// While idle, only wake word detection matters. After a wake word, the clean
// audio stream is delivered to on_audio (for Gemini) until wakeword_listen() is called.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /** Wake word detected. Called from the AFE fetch task. Keep it short. */
    void (*on_wake)(void *ctx);
    /** Clean 16 kHz mono audio while in conversation mode. speech = VAD verdict. */
    void (*on_audio)(const int16_t *pcm, size_t samples, bool speech, void *ctx);
    void *ctx;
} wakeword_config_t;

/** Load models, create the AFE and start feed/fetch tasks. */
esp_err_t wakeword_start(const wakeword_config_t *cfg);

/** Back to idle: detect the wake word, stop delivering audio. */
void wakeword_listen(void);

/** Conversation mode: stop wake word detection, deliver audio to on_audio. */
void wakeword_converse(void);
