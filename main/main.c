// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "audio.h"
#include "wakeword.h"
#include "gemini_live.h"
#include "gemini_proto.h"
#include "tools.h"
#include "appointments.h"
#include "setup_web.h"
#include "net.h"
#include "improv.h"
#include "settings.h"
#include "leds.h"
#include "storage.h"
#include "board.h"
#include "conversations.h"
#include "diag.h"
#include "prompts.h"
#include "battery.h"
#include "voicepack.h"
#include "clock.h"
#include "ota.h"
#include "intercom.h"
#include "captive.h"
#include "dsp.h"
#include "esp_system.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "main";



typedef enum { EV_WAKE, EV_SESSION_ENDED, EV_SESSION_FAILED, EV_END_REQUESTED,
               EV_RING_START, EV_RING_STOP } app_event_t;
static QueueHandle_t s_events;
static volatile bool s_in_conversation;

static void on_wake(void *ctx)
{
    leds_event(LEDS_EVENT_WAKE);
    if (clock_ringing()) {                     // "Jarvis" silences a timer or alarm
        clock_stop_ringing();
        return;
    }
    app_event_t ev = EV_WAKE;
    xQueueSend(s_events, &ev, 0);
}

static leds_state_t idle_state(void);

static void post(app_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

// Optional session features some models reject. If the setup fails, retry without
// proactive audio, then also without search; remember what had to be dropped.
static volatile int s_retry_stage;              // 0 = first try, 1 = no proactive, 2 = neither
static volatile bool s_try_search, s_try_proactive;

static void on_gemini_state(gemini_state_t st, void *ctx)
{
    switch (st) {
    case GEMINI_STATE_CONNECTING:     leds_set(LEDS_CONNECTING); break;
    case GEMINI_STATE_READY:
        if (s_retry_stage == 1) {
            ESP_LOGW(TAG, "'only answer when spoken to' isn't supported by this model; turned it off");
            settings_set_proactive(false);
        } else if (s_retry_stage == 2) {
            ESP_LOGW(TAG, "this model rejects Google Search (and proactive audio) here; turned them off");
            settings_set_search(false);
            settings_set_proactive(false);
        }
        s_retry_stage = 0;
        leds_set(LEDS_LISTENING);                                      // ready for your voice
        break;
    case GEMINI_STATE_MODEL_SPEAKING: leds_set(LEDS_SPEAKING);   break;
    case GEMINI_STATE_CLOSED:         post(EV_SESSION_ENDED);    break;
    case GEMINI_STATE_ERROR:          post(EV_SESSION_FAILED);   break;
    }
}

static void on_transcript(const char *role, const char *text, void *ctx)
{
    ESP_LOGI(TAG, "%s: %s", role, text);
    conversations_add_text(role, text);
}

/** System instruction for each conversation: persona, today's date, tool guidance, memory. */
static char *build_instruction(bool google_search, bool proactive)
{
    char now[64] = "unknown";
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year > 120) strftime(now, sizeof(now), "%A %d %B %Y, %H:%M", &tm);

    char *appts = appointments_summary(14);
    const char *tools = tools_system_instruction();
    char update_note[256] = "", newver[48], style[SETTINGS_STYLE_MAX + 80];
    settings_style_instruction(style, sizeof(style));
    if (ota_should_mention(newver, sizeof(newver))) {
        snprintf(update_note, sizeof(update_note),
                 "A software update for you is available (version %s). At the end of your first answer, "
                 "mention it once in a few words and that they can say \"update yourself\" to install it.\n",
                 newver);
    }
    size_t len = 2048 + strlen(tools) + strlen(update_note) + strlen(style) + (appts ? strlen(appts) : 0);
    char *buf = malloc(len);
    if (buf) {
        snprintf(buf, len,
            "You are %s, a friendly voice assistant running on a small speaker in the user's home. %s"
            "The user talks to you out loud and hears your replies, so speak naturally and keep "
            "answers short: one to three sentences unless they ask for more. Never use lists, "
            "markdown, emojis or URLs. The user wakes you by saying \"Jarvis\".\n"
            "Current local date and time: %s (time zone %s).\n"
            "When the user says goodbye, thanks you to finish, or says they're done, say a very "
            "short goodbye and then call end_conversation.\n"
            "%s%s"
            "The user's home location is %s.\n"
            "%s"
            "%s%s%s",
            settings_get_name(), style, now, settings_get_tz(),
            proactive ? "Only speak when the user is talking to you. If you hear background noise, a TV "
                        "or radio, or people talking to each other, stay silent. After you answer, stop "
                        "and wait: don't add follow-up questions or keep talking unless you need more "
                        "information to help. If the user starts talking while you speak, stop and listen.\n"
                      : "",
            update_note,
            settings_get_location()[0] ? settings_get_location() : "not set",
            google_search ? "Use Google Search for current information such as news, sports, "
                            "opening hours, prices and facts you're unsure about.\n" : "",
            tools,
            appts ? "\nUpcoming appointments:\n" : "", appts ? appts : "");
    }
    free(appts);
    return buf;
}

static volatile bool s_wifi_saving;          // Wi-Fi off on purpose (battery saver)
static int64_t s_last_activity_us;           // last conversation / wake / page use

/** Idle light: dark when everything's ready, amber blink when setup is missing. */
static leds_state_t idle_state(void)
{
    bool wifi_ok = net_is_connected() || (s_wifi_saving && net_has_credentials());
    return (wifi_ok && settings_has_api_key()) ? LEDS_OFF : LEDS_SETUP_NEEDED;
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
static void update_progress(int pct)
{
    char s[48];
    snprintf(s, sizeof(s), "downloading: %d%%", pct);
    diag_set_status("update", s);
}

/** Install a requested update (only ever runs when someone asked for it). */
static void do_update(void)
{
    ESP_LOGW(TAG, "installing an update, as asked");
    diag_set_status("update", "starting");
    leds_set(LEDS_CONNECTING);
    prompts_say_always(PROMPT_UPDATE_STARTED);         // "Updating now..."
    audio_wait_idle(8000);
    net_power_save(false);
    esp_err_t err = ota_install(update_progress);
    if (err == ESP_OK) {
        diag_set_status("update", "installed: restarting into the new version");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    char why[160];
    snprintf(why, sizeof(why), "failed: %s", ota_last_error());
    diag_set_status("update", why);
    net_power_save(true);
    set_light(LEDS_ERROR, PROMPT_UPDATE_FAILED);       // "Sorry, the update did not work..."
}

/**
 * A Gemini voice session needs a free 12 KB block of internal RAM for its task (and the
 * web server's Test Gemini runs alongside). Check there's room to spare and say so on
 * the diagnostics page and in the log, before a conversation fails for lack of it.
 */
#define VOICE_SESSION_BLOCK (12 * 1024)
#define MEMORY_MARGIN       (4 * 1024)

static void check_memory(void)
{
    if (!wakeword_is_idle()) return;                   // measure between conversations
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    bool ok = block >= VOICE_SESSION_BLOCK + MEMORY_MARGIN;
    char s[128];
    snprintf(s, sizeof(s), "%s: %u KB internal RAM free, largest block %u KB (a voice session needs %u KB)",
             ok ? "ok" : "LOW, voice sessions may fail", (unsigned)(free_b / 1024), (unsigned)(block / 1024),
             VOICE_SESSION_BLOCK / 1024);
    diag_set_status("memory", s);
    static bool warned;
    if (!ok && !warned) ESP_LOGW(TAG, "memory %s", s);
    warned = !ok;
}

/** After an update: keep the new version once it's up and online, or go back. */
static void check_update_outcome(void)
{
    static bool settled;
    if (settled) return;
    if (ota_on_trial()) {
        if (net_is_connected()) {
            settled = true;
            ota_confirm();
            diag_set_status("update", "updated to this version");
            prompts_say_always(PROMPT_UPDATE_DONE);    // "I have been updated..."
        } else if (esp_timer_get_time() > 5LL * 60 * 1000000 && net_has_credentials()) {
            prompts_say_always(PROMPT_UPDATE_FAILED);
            audio_wait_idle(8000);
            ota_roll_back();                           // can't reach Wi-Fi: back to the old version
        }
        return;
    }
    settled = true;
    if (ota_take_rolled_back()) {
        diag_set_status("update", "the last update didn't work; went back to this version");
        prompts_say_always(PROMPT_UPDATE_FAILED);
    } else if (ota_confirm()[0]) {
        diag_set_status("update", "updated to this version");
        prompts_say_always(PROMPT_UPDATE_DONE);
    }
}

// Without Gemini (no credit, no key, Gemini down) "Jarvis" still works: if an update is
// waiting, offer it, and a second "Jarvis" within a few seconds installs it.
static int64_t s_offer_until;

/** Play the update's "what's new" (recorded in the built-in voice; no Gemini needed). */
static void say_whats_new(void)
{
    uint8_t *enc;
    size_t n;
    if (ota_fetch_notes_audio(&enc, &n) != ESP_OK) return;
    EXT_RAM_BSS_ATTR static int16_t pcm[1024];
    for (size_t off = 0; off < n; off += 1024) {
        size_t k = n - off < 1024 ? n - off : 1024;
        for (size_t i = 0; i < k; i++) pcm[i] = dsp_ulaw_decode(enc[off + i]);
        audio_play_16k(pcm, k);
    }
    free(enc);
    audio_wait_idle(45000);
}

static void offer_update_if_any(void)
{
    ota_status_t st = ota_last_status();
    if (!st.checked || !st.ok) ota_check(&st);
    if (!st.available) return;
    audio_wait_idle(10000);                            // after the "couldn't reach Gemini" message
    say_whats_new();                                   // "Here's what's new in the update..."
    prompts_say_always(PROMPT_UPDATE_OFFER);           // "...say Jarvis again now."
    audio_wait_idle(10000);
    s_offer_until = esp_timer_get_time() + 10LL * 1000000;
    ESP_LOGI(TAG, "offered the update: say \"Jarvis\" in the next 10 s to install it");
}

static cJSON *tool_handler(const char *name, const cJSON *args)
{
    if (strcmp(name, "install_update") == 0) {
        leds_set(LEDS_THINKING);
        cJSON *r = cJSON_CreateObject();
        ota_status_t st;
        ota_check(&st);
        battery_status_t b;
        battery_read(&b);
        if (!st.available) {
            cJSON_AddStringToObject(r, "error", st.needs_usb ? "This update needs one install from the web install "
                                    "page with a USB cable." : st.ok ? "Already on the latest version." : st.why);
        } else if (battery_on_battery() && b.present && b.percent < 25) {
            cJSON_AddStringToObject(r, "error", "Battery too low to update safely: plug in the charger first.");
        } else {
            ota_request_install();             // runs once the conversation has closed
            post(EV_END_REQUESTED);
            cJSON_AddBoolToObject(r, "ok", true);
            cJSON_AddStringToObject(r, "instructions", "Say only \"Okay.\" The board announces the update itself.");
        }
        leds_set(LEDS_LISTENING);
        return r;
    }
    if (strcmp(name, "end_conversation") == 0) {
        post(EV_END_REQUESTED);              // after the goodbye has played
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        return r;
    }
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
    audio_wait_idle(10000);
    net_power_save(true);                      // radio dozes while idle
    s_last_activity_us = esp_timer_get_time();
    s_in_conversation = false;
    leds_state_t cur = leds_get();
    if (clock_ringing()) leds_set(LEDS_REMINDER);
    else if (cur != LEDS_ERROR) leds_set(idle_state());
    wakeword_listen();
}

/** Setup hotspot on: the captive Wi-Fi page replaces the setup page (Wi-Fi setup only). */
static void on_setup_ap(bool on, const char *ssid)
{
    if (on) {
        setup_web_stop();
        captive_start();
        char s[80];
        snprintf(s, sizeof(s), "can't reach Wi-Fi: setup hotspot \"%s\" is on", ssid);
        diag_set_status("wifi", s);
        prompts_say(PROMPT_WIFI_SETUP_HOTSPOT);   // "...connect your phone to the Jarvis Setup network"
    } else {
        captive_stop();
        if (!net_on_public_network()) setup_web_start();
    }
}

/** Public Wi-Fi (open network that isn't one of ours): no setup page for strangers. */
static void on_wifi_trust(bool public_network)
{
    if (public_network) {
        setup_web_stop();
        diag_set_status("setup page", "off: on a public Wi-Fi network");
    } else {
        setup_web_start();
        diag_set_status("setup page", "on");
    }
}

/** A timer or alarm started (true) or stopped (false) ringing. Runs on the clock task. */
static void on_ring(bool ringing, clock_ring_t kind, const char *label)
{
    post(ringing ? EV_RING_START : EV_RING_STOP);
}

static void on_audio(const int16_t *pcm, size_t samples, bool speech, void *ctx)
{
    gemini_live_send_audio(pcm, samples, speech);   // the client tracks the idle timeout
}

/**
 * Battery level and warnings. The level is a steady, at-rest estimate (see battery.h).
 * "Low" (10%) and "almost empty" (4%) are announced once each, only after the level has
 * stayed there for 2 minutes, and only while idle; re-armed after charging past 20%.
 */
static void check_battery(void)
{
    static bool warned_low, warned_critical;
    static int low_ticks, critical_ticks;              // consecutive 5 s checks at/below
    battery_tick(!wakeword_is_idle() || audio_is_playing());
    battery_status_t b;
    battery_read(&b);
    char s[64];
    if (b.present) {
        snprintf(s, sizeof(s), "%d%% (%.2f V at rest)", b.percent, b.volts);
    } else {
        snprintf(s, sizeof(s), "no battery detected");
    }
    diag_set_status("battery", s);
    if (!b.present) {
        low_ticks = critical_ticks = 0;
        return;
    }
    if (b.percent >= 20) {
        warned_low = warned_critical = false;
    }
    low_ticks = b.percent <= 10 ? low_ticks + 1 : 0;
    critical_ticks = b.percent <= 4 ? critical_ticks + 1 : 0;
    const int sustained = 24;                          // 24 x 5 s = 2 minutes
    if (!wakeword_is_idle() || audio_is_playing()) return;
    if (critical_ticks >= sustained && !warned_critical) {
        warned_critical = warned_low = true;
        prompts_say(PROMPT_BATTERY_CRITICAL);
    } else if (low_ticks >= sustained && !warned_low) {
        warned_low = true;
        prompts_say(PROMPT_BATTERY_LOW);
    }
}

/**
 * Voice recording jobs are started by saving the setup page (or the diagnostics page).
 * Here: report status, resume a job paused by a conversation, and say "Ready" in the
 * new voice when a job finishes.
 */
static void check_voicepack(void)
{
    char st[352];
    voicepack_status(st, sizeof(st));
    diag_set_status("voice", st);
    if (voicepack_just_finished()) {
        prompts_say(PROMPT_READY);                     // first words in the new voice
        return;
    }
    if (voicepack_pending() && !voicepack_busy() && net_is_connected() &&
        wakeword_is_idle() && !audio_is_playing()) {
        voicepack_resume();
    }
}

/** The intercom may use Gemini and the speaker: not in a conversation, test or alarm. */
static bool intercom_can_start(void)
{
    return wakeword_is_idle() && !clock_ringing();
}

/** Gemini is in use: a conversation, a self-test, voice recording or the intercom. */
static bool gemini_in_use(void)
{
    return !wakeword_is_idle() || intercom_busy() || voicepack_busy();
}

static bool device_free(void)
{
    return wakeword_is_idle() && !clock_ringing() && !intercom_busy();   // not talking, testing, ringing or announcing
}

// Interruptions are paused until reboot if Gemini keeps hearing itself (echo
// cancellation not working), so every conversation doesn't start with a glitch.
static int s_echo_count;
static bool s_echo_this_boot;

/**
 * Battery saver: on battery, turn Wi-Fi off after a few quiet minutes; turn it back
 * on when the board is plugged in or the saver is switched off ("Jarvis" also does).
 * Returns true while Wi-Fi is off on purpose.
 */
#define SAVER_IDLE_US (3LL * 60 * 1000000)
static bool battery_saver(void)
{
    bool on_batt = battery_on_battery();
    if (s_wifi_saving) {
        if (!on_batt || !settings_get_battery_saver()) {
            ESP_LOGI(TAG, "battery saver: Wi-Fi back on (%s)", on_batt ? "saver turned off" : "external power");
            s_wifi_saving = false;
            net_connect(10000);
            s_last_activity_us = esp_timer_get_time();
            return false;
        }
        diag_set_status("wifi", "off to save battery (say \"Jarvis\" to wake it)");
        return true;
    }
    int64_t now = esp_timer_get_time();
    int64_t last = s_last_activity_us > setup_web_last_activity_us() ? s_last_activity_us
                                                                      : setup_web_last_activity_us();
    if (settings_get_battery_saver() && on_batt && net_is_connected() &&
        now - last > SAVER_IDLE_US && wakeword_is_idle() && !voicepack_pending() && !audio_is_playing()) {
        ESP_LOGI(TAG, "battery saver: no one has talked to me for 3 minutes, turning Wi-Fi off");
        s_wifi_saving = true;
        net_disconnect();
        diag_set_status("wifi", "off to save battery (say \"Jarvis\" to wake it)");
        return true;
    }
    return false;
}

/** Open a Gemini Live session for this conversation. */
static esp_err_t start_session(bool google_search, bool proactive, bool retry)
{
    char key[SETTINGS_API_KEY_MAX];
    settings_get_api_key(key, sizeof(key));
    char *instruction = build_instruction(google_search, proactive);
    gemini_live_config_t cfg = {
        .api_key = key,
        .model = settings_get_model(),
        .voice = voicepack_conversation_voice(),    // same voice as the device's messages
        .system_instruction = instruction,
        .tools = tools_declarations(),
        .tool_handler = tool_handler,
        .on_state = on_gemini_state,
        .on_transcript = on_transcript,
        .idle_timeout_s = CONFIG_VA_IDLE_TIMEOUT_S,
        .barge_in = settings_get_barge_in() && !s_echo_this_boot,
        .proactive_audio = proactive,
        .google_search = google_search,
        .keep_buffered_audio = retry,
    };
    s_try_search = google_search;
    s_try_proactive = proactive;
    esp_err_t err = gemini_live_start(&cfg);
    memset(key, 0, sizeof(key));
    free(instruction);
    diag_set_status("gemini", err == ESP_OK ? "connecting" : esp_err_to_name(err));
    return err;
}

/** cJSON (Gemini messages, page data) allocates many small nodes: keep them out of internal RAM. */
static void *json_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void app_main(void)
{
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);                   // before anything builds JSON
    ESP_ERROR_CHECK(nvs_flash_init());
    ota_init();                                // is this a new version on trial?
    diag_init();                               // capture logs for the diagnostics page
    diag_set_busy_check(gemini_in_use);        // the Gemini test waits its turn
    s_events = xQueueCreate(8, sizeof(app_event_t));

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

    if (battery_init() != ESP_OK) ESP_LOGW(TAG, "battery sensing unavailable");

    ESP_ERROR_CHECK(net_init());
    net_set_allow_open(settings_get_open_wifi());
    net_on_trust_change(on_wifi_trust);
    net_on_setup_ap(on_setup_ap);
    // No automatic update checks for now (frees a task's RAM). Updates still work on
    // request: "Jarvis, update yourself" or Check for updates / Install update.
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
    bool was_online = online;
    bool wifi_expected = true;                 // false while Wi-Fi is off on purpose
    s_last_activity_us = esp_timer_get_time();
    net_power_save(true);

    if (storage_init() == ESP_OK) {
        // microSD card (optional): new recordings go there. DAT3 high selects SD mode.
        if (aerr == ESP_OK) audio_expander_set(BOARD_EXP_SD_D3, true);
        storage_sd_init();
        diag_set_status("sd card", storage_sd_status());
        ESP_ERROR_CHECK(conversations_init());
        voicepack_set_gate(device_free);       // never record during a conversation
        voicepack_init();                      // use lines recorded in Gemini's voice, if any
        diag_set_status("storage", "ok");
    } else {
        diag_set_status("storage", "mount failed");
    }
    ESP_ERROR_CHECK(appointments_init());
    clock_init(on_ring);                       // timers, alarms (saved on flash), stopwatch
    intercom_set_gate(intercom_can_start);
    if (intercom_init() != ESP_OK) ESP_LOGW(TAG, "intercom unavailable");

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
    diag_set_status("gemini", "idle");

    // Speak only now that start-up is over. Mounting storage and loading the wake word
    // models read a lot of flash, which stalls playback (the start-up message stuttered).
    // Also, by now the message is in the selected Gemini voice, not the built-in one.
    vTaskDelay(pdMS_TO_TICKS(300));            // let the microphone pipeline settle
    leds_set(idle_state());                    // amber blink explains itself out loud
    if (idle_state() == LEDS_OFF) {
        prompts_say(PROMPT_READY);
    }

    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(5000)) != pdTRUE) {
            check_battery();
            check_voicepack();
            check_update_outcome();
            static int mem_ticks;
            if (mem_ticks++ % 12 == 0) check_memory();      // once a minute
            if (device_free() && ota_take_install_request()) {
                do_update();                           // asked for on the diagnostics page
            }
            if (battery_saver()) {
                continue;                              // Wi-Fi is off on purpose: nothing to report
            }
            bool now_online = net_is_connected();
            diag_set_status("wifi", now_online ? "connected" : "not connected (retrying)");
            prompt_t reason = PROMPT_COUNT;
            // Brief drops (switching networks, a router hiccup) aren't worth announcing:
            // only treat Wi-Fi as lost after two checks in a row (~10 s).
            static int offline_checks;
            offline_checks = now_online ? 0 : offline_checks + 1;
            if (!now_online && offline_checks < 2) {
                continue;
            }
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
        case EV_RING_START:
            if (s_in_conversation) {                   // the alarm wins; the conversation ends
                ESP_LOGI(TAG, "ending the conversation: a timer or alarm went off");
                gemini_live_stop();
                back_to_listening();
            }
            s_last_activity_us = esp_timer_get_time();
            wakeword_allow_while_playing(true);        // "Jarvis" over the beeps stops it
            leds_set(LEDS_REMINDER);
            break;
        case EV_RING_STOP:
            wakeword_allow_while_playing(false);
            s_last_activity_us = esp_timer_get_time();
            if (leds_get() == LEDS_REMINDER) leds_set(idle_state());
            break;
        case EV_WAKE: {
            if (clock_ringing()) break;                // that "Jarvis" stopped the ringing
            if (s_offer_until && esp_timer_get_time() < s_offer_until) {
                s_offer_until = 0;                     // "Jarvis" again: yes, install it
                if (ota_last_status().available) {
                    ota_take_install_request();
                    do_update();
                    break;
                }
            }
            s_offer_until = 0;
            ESP_LOGI(TAG, "wake: starting conversation");
            if (intercom_busy()) {
                intercom_abort();                      // someone wants to talk: drop the announcement
                for (int i = 0; i < 80 && intercom_busy(); i++) vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (voicepack_busy()) {
                voicepack_abort();                     // the user comes first; it resumes later
                for (int i = 0; i < 60 && voicepack_busy(); i++) vTaskDelay(pdMS_TO_TICKS(50));
            }
            s_retry_stage = 0;
            s_in_conversation = true;
            wakeword_converse();                       // stop wake detection while we talk
            prompts_wake_ack();                        // "Yes?" / "I'm listening." / "How can I help?"
            leds_set(LEDS_CONNECTING);
            s_last_activity_us = esp_timer_get_time();
            wifi_expected = true;
            bool was_saving = s_wifi_saving;
            s_wifi_saving = false;                     // battery saver: Wi-Fi back on
            // Speech is buffered meanwhile, so the user doesn't have to wait.
            if (net_connect(was_saving ? 15000 : 8000) != ESP_OK) {
                ESP_LOGW(TAG, "no Wi-Fi");
                set_light(LEDS_ERROR, PROMPT_NO_INTERNET);
                back_to_listening();
                break;
            }
            was_online = true;
            net_power_save(false);                     // full-speed radio for smooth audio
            if (!settings_has_api_key()) {
                set_light(LEDS_SETUP_NEEDED, PROMPT_NEED_API_KEY);
                back_to_listening();
                offer_update_if_any();
                break;
            }
            conversations_session_begin();             // audio now flows to Gemini (buffered)
            if (start_session(settings_get_search(), settings_get_proactive(), false) != ESP_OK) {
                set_light(LEDS_ERROR, PROMPT_GEMINI_FAILED);
                back_to_listening();
                offer_update_if_any();
            }
            break;
        }
        case EV_END_REQUESTED:
            audio_wait_idle(15000);                    // let the goodbye finish
            /* fall through */
        case EV_SESSION_ENDED:
            if (gemini_live_echo_detected()) {
                if (++s_echo_count >= 2 && !s_echo_this_boot) {
                    s_echo_this_boot = true;
                    diag_set_status("interruptions", "paused until restart: Gemini heard its own voice in two "
                                    "conversations (echo cancellation isn't working; check Microphone levels)");
                } else {
                    diag_set_status("interruptions", "paused for one conversation: Gemini heard its own voice");
                }
            } else if (!s_echo_this_boot) {
                s_echo_count = 0;                      // only consecutive echoes count
                diag_set_status("interruptions", settings_get_barge_in() ? "on" : "off (setup page)");
            }
            gemini_live_stop();
            diag_set_status("gemini", "idle (last conversation ended normally)");
            ESP_LOGI(TAG, "conversation ended");
            back_to_listening();
            if (ota_take_install_request()) do_update();   // "Jarvis, update yourself"
            break;
        case EV_SESSION_FAILED: {
            char why[224];
            int smax, sn = net_sockets_in_use(&smax);
            snprintf(why, sizeof(why), "failed: %s (network sockets in use: %d of %d)",
                     gemini_live_last_error(), sn, smax);
            ESP_LOGW(TAG, "Gemini %s", why);
            diag_set_status("gemini", why);
            gemini_live_stop();
            if (gemini_error_is_account(gemini_live_last_error())) {
                // Out of credit, over quota or a bad key: fewer features won't help.
                snprintf(why, sizeof(why), "your Gemini account or key: %s", gemini_live_last_error());
                diag_set_status("gemini", why);
                s_retry_stage = 0;
                set_light(LEDS_ERROR, PROMPT_GEMINI_FAILED);
                back_to_listening();
                offer_update_if_any();
                break;
            }
            // Some models reject optional features in the setup: retry without them,
            // resending the speech buffered by the failed attempt.
            if (s_retry_stage == 0 && s_try_proactive) {
                ESP_LOGW(TAG, "retrying without 'only answer when spoken to'");
                s_retry_stage = 1;
                if (start_session(s_try_search, false, true) == ESP_OK) break;
            }
            if (s_retry_stage <= 1 && s_try_search) {
                ESP_LOGW(TAG, "retrying without Google Search");
                s_retry_stage = 2;
                if (start_session(false, false, true) == ESP_OK) break;
            }
            s_retry_stage = 0;
            set_light(LEDS_ERROR, PROMPT_GEMINI_FAILED);
            back_to_listening();
            offer_update_if_any();
            break;
        }
        }
    }
}
