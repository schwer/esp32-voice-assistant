// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "audio.h"
#include "gemini_live.h"
#include "tools.h"
#include "appointments.h"
#include "setup_web.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // TODO: Wi-Fi station connect (CONFIG_VA_WIFI_SSID / PASSWORD)
    // TODO: SNTP sync + setenv("TZ", CONFIG_VA_TIMEZONE) + tzset()

    ESP_ERROR_CHECK(appointments_init());   // LittleFS-backed store
    ESP_ERROR_CHECK(audio_init());          // I2S codec, mic 16 kHz, spk 24 kHz
    ESP_ERROR_CHECK(setup_web_start());     // http://assistant.local (setup mode)

    gemini_live_config_t cfg = GEMINI_LIVE_CONFIG_DEFAULT();
    cfg.tool_handler = tools_handle_call;   // routes toolCall -> appointments etc.
    ESP_ERROR_CHECK(gemini_live_start(&cfg));

    ESP_LOGI(TAG, "voice assistant started");
}
