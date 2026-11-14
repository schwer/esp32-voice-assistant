// SPDX-License-Identifier: MIT
//
// Board audio: ES8311 DAC -> speaker amp (enabled via TCA9555), ES7210 ADC <- mics.
// Both codecs share one I2S clock, so everything runs at BOARD_I2S_SAMPLE_RATE (16 kHz).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/** Init I2C, I2S, codecs and the playback task. ESP_OK if the speaker works. */
esp_err_t audio_init(void);
/** Shared I2C bus (codecs, IO expander, RTC). NULL before audio_init(). */
i2c_master_bus_handle_t audio_i2c_bus(void);

/** Recent level of each input channel in dBFS (diagnostics). Returns channel count. */
int audio_input_levels(float *db, int max);
/** True once the microphones are running. */
bool audio_mic_ready(void);
/**
 * Read interleaved raw frames (mics + playback reference) for the AFE.
 * Channel order must match CONFIG_VA_AFE_INPUT_FORMAT. Returns frames read.
 */
size_t audio_read_raw(int16_t *buf, size_t frames, int channels, uint32_t timeout_ticks);

/** Queue 16 kHz mono PCM for playback. Blocks while the buffer is full. */
esp_err_t audio_play_16k(const int16_t *pcm, size_t samples);
/** Queue 24 kHz mono PCM from Gemini (resampled to 16 kHz). */
esp_err_t audio_play(const int16_t *pcm, size_t samples);
/** Drop queued playback (barge-in / "interrupted"). */
void audio_flush_playback(void);
/** True while audio is queued or playing. */
bool audio_is_playing(void);
/** Output volume 0-100. */
void audio_set_volume(int volume);
