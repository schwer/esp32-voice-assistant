// SPDX-License-Identifier: MIT
#include "wakeword.h"
// TODO: engine selected via Kconfig (ESP-SR WakeNet or microWakeWord).
esp_err_t wakeword_start(wakeword_cb_t cb, void *ctx) { (void)cb; (void)ctx; return ESP_OK; }
void wakeword_pause(bool p) { (void)p; }
