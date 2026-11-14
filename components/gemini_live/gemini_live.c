// SPDX-License-Identifier: MIT
#include "gemini_live.h"
// TODO: esp_websocket_client over TLS, setup msg (model, AUDIO modality, voice, tools,
//       contextWindowCompression), realtimeInput.audio base64 streaming,
//       serverContent audio -> audio_play(), interrupted -> audio_flush_playback(),
//       toolCall -> tool_handler -> toolResponse, goAway / session resumption.
//       Setup must include tools_declarations(), tools_system_instruction(), and
//       inputAudioTranscription/outputAudioTranscription; feed transcription text to
//       conversations_add_text("user"/"assistant", ...) for saved conversations.
esp_err_t gemini_live_start(const gemini_live_config_t *cfg) { (void)cfg; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gemini_live_stop(void) { return ESP_OK; }
esp_err_t gemini_live_send_audio(const int16_t *pcm, size_t n) { (void)pcm; (void)n; return ESP_OK; }
bool gemini_live_ready(void) { return false; }
esp_err_t gemini_live_send_text(const char *text) { (void)text; return ESP_OK; }
