// SPDX-License-Identifier: MIT
#include "gemini_live.h"
// TODO: esp_websocket_client over TLS, setup msg (model, AUDIO modality, voice, tools,
//       contextWindowCompression), realtimeInput.audio base64 streaming,
//       serverContent audio -> audio_play(), interrupted -> audio_flush_playback(),
//       toolCall -> tool_handler -> toolResponse, goAway / session resumption.
esp_err_t gemini_live_start(const gemini_live_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t gemini_live_stop(void) { return ESP_OK; }
esp_err_t gemini_live_send_text(const char *text) { (void)text; return ESP_OK; }
