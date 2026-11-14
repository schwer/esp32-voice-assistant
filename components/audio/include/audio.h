// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** Init I2S + codecs at BOARD_I2S_SAMPLE_RATE (16 kHz, shared clock). */
esp_err_t audio_init(void);
/**
 * Read interleaved raw frames (mics + playback reference) for the AFE.
 * Channel order must match CONFIG_VA_AFE_INPUT_FORMAT. Returns frames read.
 */
size_t audio_read_raw(int16_t *buf, size_t frames, int channels, uint32_t timeout_ticks);
/** Queue 24 kHz mono PCM from Gemini for playback (resampled + jitter-buffered). */
esp_err_t audio_play(const int16_t *pcm, size_t samples);
/** Drop queued playback (barge-in / "interrupted"). */
void audio_flush_playback(void);
