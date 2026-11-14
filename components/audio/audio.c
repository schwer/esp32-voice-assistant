// SPDX-License-Identifier: MIT
#include "audio.h"
// TODO: esp_codec_dev board setup, ESP-SR AFE (AEC + VAD), playback ring buffer in PSRAM
// Not implemented yet: report that honestly so the wake word engine isn't started
// without an audio source (it would spin on an empty buffer).
esp_err_t audio_init(void) { return ESP_ERR_NOT_SUPPORTED; }
size_t audio_read_raw(int16_t *buf, size_t n, int ch, uint32_t t) { (void)buf; (void)n; (void)ch; (void)t; return 0; }
esp_err_t audio_play(const int16_t *pcm, size_t n) { (void)pcm; (void)n; return ESP_OK; }
void audio_flush_playback(void) {}
