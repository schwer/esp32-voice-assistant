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

    // TODO: load settings from NVS (written by the setup page / web flasher)
    // TODO: restore time from the PCF85063 RTC, set TZ

    ESP_ERROR_CHECK(appointments_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(setup_web_start());

    wakeword_config_t wcfg = { .on_wake = on_wake, .on_audio = on_audio };
    ESP_ERROR_CHECK(wakeword_start(&wcfg));

    for (;;) {
        app_event_t ev;
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        switch (ev) {
        case EV_WAKE: {
            ESP_LOGI(TAG, "wake: starting conversation");
            // TODO: chime, Wi-Fi fast reconnect, SNTP if stale
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
            // TODO: Wi-Fi off if CONFIG_VA_WIFI_OFF_WHEN_IDLE
            wakeword_listen();
            break;
        }
    }
}
