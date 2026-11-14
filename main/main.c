// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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
#include "leds.h"
#include "storage.h"
#include "conversations.h"
#include "diag.h"
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

static void on_gemini_state(gemini_state_t st, void *ctx)
{
    switch (st) {
    case GEMINI_STATE_CONNECTING:     leds_set(LEDS_CONNECTING); break;
    case GEMINI_STATE_READY:          leds_set(LEDS_LISTENING);  break;   // ready for your voice
    case GEMINI_STATE_MODEL_SPEAKING: leds_set(LEDS_SPEAKING);   break;
    case GEMINI_STATE_CLOSED:         leds_set(LEDS_OFF);        break;
    case GEMINI_STATE_ERROR:          leds_set(LEDS_ERROR);      break;
    }
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
    diag_init();                               // capture logs for the diagnostics page
    s_events = xQueueCreate(4, sizeof(app_event_t));

    ESP_ERROR_CHECK(settings_init());
    if (leds_init() == ESP_OK) {
        leds_set(LEDS_CONNECTING);             // booting / joining Wi-Fi
    }
    setenv("TZ", settings_get_tz(), 1);
    tzset();
    // TODO: restore time from the PCF85063 RTC

    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(improv_start());           // Wi-Fi setup from the web flasher

    // Setup page first, so the device stays configurable even if audio fails.
    // It serves on whatever IP Wi-Fi gets, whenever that happens.
    if (setup_web_start() != ESP_OK) {
        ESP_LOGE(TAG, "setup page failed to start");
    }

    // Boot indicator: blue pulse until Wi-Fi is up (it keeps retrying in the
    // background), then one rainbow cycle. Red blinks only if there are no Wi-Fi
    // settings, or it's still offline after 30 s.
    bool online = false;
    if (net_has_credentials()) {
        net_connect(10000);
        for (int i = 0; i < 40 && !net_is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        online = net_is_connected();
    } else {
        ESP_LOGW(TAG, "no Wi-Fi settings: use the web flasher to set them");
    }
    diag_set_status("wifi", online ? "connected" : "not connected (retrying)");
    if (online) {
        leds_set(LEDS_LISTENING);
        vTaskDelay(pdMS_TO_TICKS(2000));
        leds_set(LEDS_OFF);
    } else {
        leds_set(LEDS_ERROR);
    }

    if (storage_init() == ESP_OK) {
        ESP_ERROR_CHECK(conversations_init());
        diag_set_status("storage", "ok");
    } else {
        diag_set_status("storage", "mount failed");
    }
    ESP_ERROR_CHECK(appointments_init());

    // Audio problems are logged, not fatal: a reboot loop would hide the setup page.
    wakeword_config_t wcfg = { .on_wake = on_wake, .on_audio = on_audio };
    esp_err_t aerr = audio_init();
    if (aerr == ESP_OK) {
        aerr = wakeword_start(&wcfg);
    }
    if (aerr != ESP_OK) {
        ESP_LOGW(TAG, "audio/wake word not running (%s); setup page still available",
                 esp_err_to_name(aerr));
        diag_set_status("audio", aerr == ESP_ERR_NOT_SUPPORTED ? "not implemented yet" : esp_err_to_name(aerr));
        diag_set_status("wake word", "waiting for audio");
    } else {
        diag_set_status("audio", "ok");
        diag_set_status("wake word", "listening for \"Jarvis\"");
    }
    diag_set_status("gemini", "not implemented yet");

    for (;;) {
        app_event_t ev;
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        switch (ev) {
        case EV_WAKE: {
            ESP_LOGI(TAG, "wake: starting conversation");
            leds_set(LEDS_CONNECTING);
            // TODO: chime, SNTP if stale
            if (net_connect(8000) != ESP_OK) {
                ESP_LOGW(TAG, "no Wi-Fi");      // TODO: spoken error
                leds_set(LEDS_ERROR);
                break;
            }
            conversations_session_begin();
            wakeword_converse();                       // audio now flows to Gemini (buffered)
            gemini_live_config_t cfg = GEMINI_LIVE_CONFIG_DEFAULT();
            cfg.tool_handler = tools_handle_call;
            cfg.on_state = on_gemini_state;
            cfg.system_instruction_extra = appointments_summary(14);
            if (gemini_live_start(&cfg) != ESP_OK) {
                leds_set(LEDS_ERROR);
                wakeword_listen();
            }
            break;
        }
        case EV_SESSION_ENDED:
            gemini_live_stop();
            leds_set(LEDS_OFF);
#if CONFIG_VA_WIFI_OFF_WHEN_IDLE
            net_disconnect();
#endif
            wakeword_listen();
            break;
        }
    }
}
