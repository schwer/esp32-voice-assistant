// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** Init I2S + codec. Mic: 16 kHz mono s16. Speaker: 24 kHz mono s16. */
esp_err_t audio_init(void);
/** Read captured mic PCM (after AEC). Returns bytes read. */
size_t audio_read_mic(int16_t *buf, size_t max_samples, uint32_t timeout_ms);
/** Queue PCM for playback (jitter-buffered). */
esp_err_t audio_play(const int16_t *pcm, size_t samples);
/** Drop queued playback (barge-in / "interrupted"). */
void audio_flush_playback(void);
