// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "audio.h"
#include "wakeword.h"
#include "gemini_live.h"
#include "tools.h"
#include "appointments.h"
#include "setup_web.h"
#include "net.h"
#include "improv.h"
#include "settings.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "main";

typedef enum { EV_WAKE, EV_SESSION_ENDED } app_event_t;
static QueueHandle_t s_events;

static void on_wake(void *ctx)
{
    app_event_t ev = EV_WAKE;
    xQueueSend(s_events, &ev, 0);
}

static void on_audio(const int16_t *pcm, size_t samples, bool speech, void *ctx)
{
    // TODO: track silence for CONFIG_VA_IDLE_TIMEOUT_S, then post EV_SESSION_ENDED
    (void)speech;
    gemini_live_send_audio(pcm, samples);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    s_events = xQueueCreate(4, sizeof(app_event_t));

    ESP_ERROR_CHECK(settings_init());
    setenv("TZ", settings_get_tz(), 1);
    tzset();
    // TODO: restore time from the PCF85063 RTC

    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(improv_start());           // Wi-Fi setup from the web flasher
    if (net_has_credentials()) {
        net_connect(10000);                    // first boot: sync time, show setup URL
    }

    // Setup page first, so the device stays configurable even if audio fails.
    if (setup_web_start() != ESP_OK) {
        ESP_LOGE(TAG, "setup page failed to start");
    }

    ESP_ERROR_CHECK(appointments_init());

    // Audio problems are logged, not fatal: a reboot loop would hide the setup page.
    wakeword_config_t wcfg = { .on_wake = on_wake, .on_audio = on_audio };
    if (audio_init() != ESP_OK || wakeword_start(&wcfg) != ESP_OK) {
        ESP_LOGE(TAG, "audio/wake word unavailable; setup page still running");
    }

    for (;;) {
        app_event_t ev;
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        switch (ev) {
        case EV_WAKE: {
            ESP_LOGI(TAG, "wake: starting conversation");
            // TODO: chime, SNTP if stale
            if (net_connect(8000) != ESP_OK) {
                ESP_LOGW(TAG, "no Wi-Fi");      // TODO: spoken/LED error
                break;
            }
            wakeword_converse();                       // audio now flows to Gemini (buffered)
            gemini_live_config_t cfg = GEMINI_LIVE_CONFIG_DEFAULT();
            cfg.tool_handler = tools_handle_call;
            cfg.system_instruction_extra = appointments_summary(14);
            if (gemini_live_start(&cfg) != ESP_OK) {
                wakeword_listen();
            }
            break;
        }
        case EV_SESSION_ENDED:
            gemini_live_stop();
#if CONFIG_VA_WIFI_OFF_WHEN_IDLE
            net_disconnect();
#endif
            wakeword_listen();
            break;
        }
    }
}
