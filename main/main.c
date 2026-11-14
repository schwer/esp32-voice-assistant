// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
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
#include "prompts.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "main";

typedef enum { EV_WAKE, EV_SESSION_ENDED } app_event_t;
static QueueHandle_t s_events;

static void on_wake(void *ctx)
{
    leds_event(LEDS_EVENT_WAKE);
    app_event_t ev = EV_WAKE;
    xQueueSend(s_events, &ev, 0);
}

static leds_state_t idle_state(void);

static void on_gemini_state(gemini_state_t st, void *ctx)
{
    switch (st) {
    case GEMINI_STATE_CONNECTING:     leds_set(LEDS_CONNECTING); break;
    case GEMINI_STATE_READY:          leds_set(LEDS_LISTENING);  break;   // ready for your voice
    case GEMINI_STATE_MODEL_SPEAKING: leds_set(LEDS_SPEAKING);   break;
    case GEMINI_STATE_CLOSED:         leds_set(idle_state());    break;
    case GEMINI_STATE_ERROR:          leds_set(LEDS_ERROR);      break;
    }
}

/** Idle light: dark when everything's ready, amber blink when setup is missing. */
static leds_state_t idle_state(void)
{
    return (net_is_connected() && settings_has_api_key()) ? LEDS_OFF : LEDS_SETUP_NEEDED;
}

// ---- Say what a blinking light means -------------------------------------
// Whenever the ring switches to a blinking pattern, the device explains it out loud.
// Callers can give a more specific reason (e.g. "I lost the Wi-Fi connection").

static volatile int s_light_reason = PROMPT_COUNT;   // PROMPT_COUNT = use the default

static void set_light(leds_state_t state, prompt_t reason)
{
    s_light_reason = reason;
    leds_set(state);
    s_light_reason = PROMPT_COUNT;
}

static void on_light_change(leds_state_t from, leds_state_t to)
{
    prompt_t reason = (prompt_t)s_light_reason;
    switch (to) {
    case LEDS_SETUP_NEEDED:
        prompts_say(reason != PROMPT_COUNT ? reason
                    : net_is_connected() ? PROMPT_NEED_API_KEY : PROMPT_NEED_WIFI);
        break;
    case LEDS_ERROR:
        prompts_say(reason != PROMPT_COUNT ? reason : PROMPT_ERROR);
        break;
    default:
        break;      // steady patterns (listening, thinking, speaking) stay quiet
    }
}

/** Wraps tool calls with light feedback (thinking, then saved/deleted). */
static cJSON *tool_handler(const char *name, const cJSON *args)
{
    leds_set(LEDS_THINKING);
    cJSON *r = tools_handle_call(name, args);
    bool failed = cJSON_GetObjectItem(r, "error") != NULL;
    if (!failed && strncmp(name, "save_", 5) == 0) {
        leds_event(LEDS_EVENT_SAVED);
    } else if (!failed && strncmp(name, "delete_", 7) == 0) {
        leds_event(LEDS_EVENT_DELETED);
    }
    leds_set(LEDS_LISTENING);             // the model's reply follows
    return r;
}

/** Wait for anything we're saying to finish, then listen for "Jarvis" again. */
static void back_to_listening(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));               // let queued prompts start
    for (int i = 0; i < 200 && audio_is_playing(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(300));               // tail of the speaker's echo
    leds_state_t cur = leds_get();
    if (cur != LEDS_ERROR) leds_set(idle_state());
    wakeword_listen();
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
    leds_on_change(on_light_change);
    setenv("TZ", settings_get_tz(), 1);
    tzset();
    // TODO: restore time from the PCF85063 RTC

    // Speaker early, so status messages work during setup. Audio failures are
    // logged, not fatal: a reboot loop would hide the setup page.
    esp_err_t aerr = audio_init();
    if (aerr == ESP_OK) {
        prompts_init();
        diag_set_status("speaker", "ok");
    } else {
        diag_set_status("speaker", esp_err_to_name(aerr));
    }

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
    }
    leds_set(idle_state());                    // amber blink explains itself out loud
    if (idle_state() == LEDS_OFF) {
        prompts_say(PROMPT_READY);
    }
    bool was_online = online;
    bool wifi_expected = true;                 // false while Wi-Fi is off to save battery

    if (storage_init() == ESP_OK) {
        ESP_ERROR_CHECK(conversations_init());
        diag_set_status("storage", "ok");
    } else {
        diag_set_status("storage", "mount failed");
    }
    ESP_ERROR_CHECK(appointments_init());

    // Wake word needs the microphones.
    wakeword_config_t wcfg = { .on_wake = on_wake, .on_audio = on_audio };
    if (aerr == ESP_OK && audio_mic_ready()) {
        aerr = wakeword_start(&wcfg);
    } else if (aerr == ESP_OK) {
        aerr = ESP_ERR_NOT_SUPPORTED;
    }
    if (aerr != ESP_OK) {
        ESP_LOGW(TAG, "wake word not running (%s); setup page still available",
                 esp_err_to_name(aerr));
        diag_set_status("microphones", audio_mic_ready() ? "ok" : "not responding");
        diag_set_status("wake word", "not running (see log)");
    } else {
        diag_set_status("microphones", "ok");
        diag_set_status("wake word", "listening for \"Jarvis\"");
    }
    diag_set_status("gemini", "conversations not implemented yet");

    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(5000)) != pdTRUE) {
            bool now_online = net_is_connected();
            diag_set_status("wifi", now_online ? "connected" : "not connected (retrying)");
            prompt_t reason = PROMPT_COUNT;
            if (wifi_expected && was_online && !now_online) {
                reason = PROMPT_WIFI_LOST;
            } else if (!was_online && now_online && settings_has_api_key()) {
                prompts_say(PROMPT_WIFI_BACK);
            }
            // Keep the idle light in sync with Wi-Fi / API key changes.
            leds_state_t cur = leds_get();
            if (cur == LEDS_OFF || cur == LEDS_SETUP_NEEDED) {
                leds_state_t want = idle_state();
                if (want != cur) {
                    set_light(want, reason);
                } else if (want == LEDS_SETUP_NEEDED && !was_online && now_online) {
                    prompts_say(PROMPT_NEED_API_KEY);   // online now, still no key
                }
            }
            was_online = now_online;
            static int ticks;
            if (!net_is_connected() || ++ticks % 12 == 0) {
                net_log_status();          // every 5 s while offline, every minute when online
            }
            continue;
        }
        switch (ev) {
        case EV_WAKE: {
            ESP_LOGI(TAG, "wake: starting conversation");
            wakeword_converse();                       // stop wake detection while we talk
            prompts_wake_ack();                        // "Yes?" / "I'm listening." / "How can I help?"
            leds_set(LEDS_CONNECTING);
            // TODO: chime, SNTP if stale
            wifi_expected = true;
            if (net_connect(8000) != ESP_OK) {
                ESP_LOGW(TAG, "no Wi-Fi");
                set_light(LEDS_ERROR, PROMPT_NO_INTERNET);
                back_to_listening();
                break;
            }
            was_online = true;
            conversations_session_begin();             // audio now flows to Gemini (buffered)
            gemini_live_config_t cfg = GEMINI_LIVE_CONFIG_DEFAULT();
            cfg.tool_handler = tool_handler;
            cfg.on_state = on_gemini_state;
            cfg.system_instruction_extra = appointments_summary(14);
            esp_err_t gerr = gemini_live_start(&cfg);
            if (gerr == ESP_ERR_NOT_SUPPORTED) {
                // Conversations aren't built yet: say so, then listen for "Jarvis" again.
                leds_set(LEDS_SPEAKING);
                prompts_say_always(PROMPT_NOT_READY);
                back_to_listening();
            } else if (gerr != ESP_OK) {
                set_light(LEDS_ERROR, PROMPT_GEMINI_FAILED);
                back_to_listening();
            }
            break;
        }
        case EV_SESSION_ENDED:
            gemini_live_stop();
            leds_set(idle_state());
#if CONFIG_VA_WIFI_OFF_WHEN_IDLE
            wifi_expected = false;
            net_disconnect();
            was_online = false;
#endif
            wakeword_listen();
            break;
        }
    }
}
