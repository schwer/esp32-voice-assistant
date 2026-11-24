// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "settings.h"
#include "gemini_live.h"
#include "gemini_proto.h"
#include "freertos/semphr.h"
#include "net.h"
#include "storage.h"
#include "conversations.h"
#include "audio.h"
#include "battery.h"
#include "sdkconfig.h"
#include "diag.h"

static const char *TAG = "diag";

// ---------- log ring buffer ----------

#define LOG_RING_SIZE (8 * 1024)

static char *s_ring;
static size_t s_head;          // next write position
static bool s_wrapped;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;

static int log_hook(const char *fmt, va_list args)
{
    char line[200];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n > 0 && s_ring) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        portENTER_CRITICAL(&s_ring_mux);
        for (size_t i = 0; i < len; i++) {
            s_ring[s_head++] = line[i];
            if (s_head == LOG_RING_SIZE) {
                s_head = 0;
                s_wrapped = true;
            }
        }
        portEXIT_CRITICAL(&s_ring_mux);
    }
    return s_orig_vprintf(fmt, args);
}

esp_err_t diag_init(void)
{
    s_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    s_orig_vprintf = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

size_t diag_logs(char *buf, size_t len)
{
    if (!s_ring || len == 0) {
        return 0;
    }
    size_t out = 0;
    portENTER_CRITICAL(&s_ring_mux);
    size_t avail = s_wrapped ? LOG_RING_SIZE : s_head;
    size_t start = s_wrapped ? s_head : 0;
    size_t skip = avail > len - 1 ? avail - (len - 1) : 0;   // keep the newest
    for (size_t i = skip; i < avail; i++) {
        buf[out++] = s_ring[(start + i) % LOG_RING_SIZE];
    }
    portEXIT_CRITICAL(&s_ring_mux);
    buf[out] = 0;
    return out;
}

// ---------- module status ----------

#define MAX_STATUS 12
static struct { char module[16]; char status[160]; } s_status[MAX_STATUS];
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;

void diag_set_status(const char *module, const char *status)
{
    portENTER_CRITICAL(&s_status_mux);
    int slot = -1;
    for (int i = 0; i < MAX_STATUS; i++) {
        if (strcmp(s_status[i].module, module) == 0) { slot = i; break; }
        if (slot < 0 && !s_status[i].module[0]) slot = i;
    }
    if (slot >= 0) {
        strlcpy(s_status[slot].module, module, sizeof(s_status[slot].module));
        strlcpy(s_status[slot].status, status, sizeof(s_status[slot].status));
    }
    portEXIT_CRITICAL(&s_status_mux);
}

// ---------- snapshot ----------

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_EXT:       return "external reset";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (low power)";
    case ESP_RST_USB:       return "USB reset";
    default:                return "unknown";
    }
}

static cJSON *add_obj(cJSON *parent, const char *name)
{
    return cJSON_AddObjectToObject(parent, name);
}

cJSON *diag_snapshot(void)
{
    cJSON *root = cJSON_CreateObject();

    // Device
    cJSON *dev = add_obj(root, "device");
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(dev, "firmware", app->version);
    cJSON_AddStringToObject(dev, "built", built);
    cJSON_AddStringToObject(dev, "esp_idf", esp_get_idf_version());
    cJSON_AddNumberToObject(dev, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(dev, "cores", chip.cores);
    cJSON_AddNumberToObject(dev, "flash_mb", flash / (1024 * 1024));
    cJSON_AddStringToObject(dev, "mac", macs);
    cJSON_AddNumberToObject(dev, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(dev, "last_reset", reset_reason());

    // Memory
    cJSON *mem = add_obj(root, "memory");
    cJSON_AddNumberToObject(mem, "internal_total_kb", heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "internal_free_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "internal_min_free_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "internal_largest_block_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "psram_total_kb", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(mem, "psram_free_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(mem, "psram_min_free_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
    int smax, sn = net_sockets_in_use(&smax);
    char socks[24];
    snprintf(socks, sizeof(socks), "%d of %d", sn, smax);
    cJSON_AddStringToObject(mem, "network_sockets_in_use", socks);
    // Spare stack per task (bytes never used), smallest first: shows where RAM could be trimmed.
    UBaseType_t nt = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = heap_caps_malloc(nt * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ts) {
        nt = uxTaskGetSystemState(ts, nt, NULL);
        for (UBaseType_t i = 0; i < nt; i++) {           // sort by spare stack
            for (UBaseType_t j = i + 1; j < nt; j++) {
                if (ts[j].usStackHighWaterMark < ts[i].usStackHighWaterMark) {
                    TaskStatus_t t = ts[i]; ts[i] = ts[j]; ts[j] = t;
                }
            }
        }
        char *list = calloc(1, 1024);
        for (UBaseType_t i = 0; list && i < nt; i++) {
            size_t used = strlen(list);
            snprintf(list + used, 1024 - used, "%s%s %u", i ? ", " : "", ts[i].pcTaskName,
                     (unsigned)ts[i].usStackHighWaterMark);
        }
        if (list) cJSON_AddStringToObject(mem, "spare_stack_bytes", list);
        free(list);
        free(ts);
    }

    // Wi-Fi
    cJSON *wifi = add_obj(root, "wifi");
    wifi_ap_record_t ap;
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cJSON_AddBoolToObject(wifi, "connected", connected);
    if (connected) {
        cJSON_AddStringToObject(wifi, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi_dbm", ap.rssi);
        cJSON_AddStringToObject(wifi, "signal",
            ap.rssi >= -60 ? "good" : ap.rssi >= -70 ? "fair" : "weak");
        cJSON_AddNumberToObject(wifi, "channel", ap.primary);
        cJSON_AddStringToObject(wifi, "kind", net_on_public_network() ? "public (open) network" : "one of your networks");
    }
    char nets[NET_MAX_SAVED][NET_SSID_MAX];
    int nn = net_saved_networks(nets, NET_MAX_SAVED);
    char list[NET_MAX_SAVED * (NET_SSID_MAX + 2)] = "";
    for (int i = 0; i < nn; i++) {
        if (i) strlcat(list, ", ", sizeof(list));
        strlcat(list, nets[i], sizeof(list));
    }
    cJSON_AddStringToObject(wifi, "saved_networks", nn ? list : "none");
    cJSON_AddBoolToObject(wifi, "free_wifi_allowed", settings_get_open_wifi());
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(wifi, "ip", buf);
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.gw));
        cJSON_AddStringToObject(wifi, "gateway", buf);
    }

    // Time
    cJSON *tm_o = add_obj(root, "time");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    bool synced = tm.tm_year > 120;
    cJSON_AddBoolToObject(tm_o, "synced", synced);
    if (synced) {
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        cJSON_AddStringToObject(tm_o, "local", buf);
    }
    cJSON_AddStringToObject(tm_o, "timezone", settings_get_tz());

    // Settings (no secrets)
    cJSON *set = add_obj(root, "settings");
    cJSON_AddBoolToObject(set, "api_key_set", settings_has_api_key());
    cJSON_AddStringToObject(set, "assistant_name", settings_get_name());

    // Storage
    cJSON *st = add_obj(root, "storage");
    size_t total = 0, used = 0;
    bool mounted = storage_info(&total, &used) == ESP_OK;
    cJSON_AddBoolToObject(st, "mounted", mounted);
    if (mounted) {
        cJSON_AddNumberToObject(st, "used_kb", used / 1024);
        cJSON_AddNumberToObject(st, "total_kb", total / 1024);
    }
    cJSON_AddNumberToObject(st, "saved_conversations", conversations_count());
    cJSON_AddStringToObject(st, "sd_card", storage_sd_status());
    cJSON_AddStringToObject(st, "new_recordings_go_to", storage_sd_ready() ? "microSD card" : "board memory");

    // Battery: what the assistant reports (level, trend, time left, health) + raw pins.
    battery_status_t bat;
    battery_read(&bat);
    cJSON *rep = battery_report();
    cJSON *b = add_obj(root, "battery");
    bool present = cJSON_IsTrue(cJSON_GetObjectItem(rep, "battery_connected"));
    if (present) {
        char v[48];
        snprintf(v, sizeof(v), "%d%%", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(rep, "percent")));
        cJSON_AddStringToObject(b, "level", v);
        snprintf(v, sizeof(v), "%s V (at rest, 2-minute average)", cJSON_GetStringValue(cJSON_GetObjectItem(rep, "voltage")));
        cJSON_AddStringToObject(b, "voltage", v);
        const char *trend = cJSON_GetStringValue(cJSON_GetObjectItem(rep, "trend"));
        if (trend) cJSON_AddStringToObject(b, "status", trend);
        cJSON *hl = cJSON_GetObjectItem(rep, "estimated_hours_left");
        if (cJSON_IsNumber(hl)) {
            snprintf(v, sizeof(v), "about %.1f hours", hl->valuedouble);
            cJSON_AddStringToObject(b, "time_left", v);
        }
        const char *health = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(rep, "health"), "assessment"));
        if (health) cJSON_AddStringToObject(b, "health", health);
        snprintf(v, sizeof(v), "%.2f V (noise +/- %.0f mV)", bat.last_raw_v, bat.spread_mv);
        cJSON_AddStringToObject(b, "latest_single_reading", v);
        snprintf(v, sizeof(v), "GPIO%d, %.0f:1 divider%s", bat.gpio, bat.divider, bat.locked ? "" : " (confirming)");
        cJSON_AddStringToObject(b, "sense_pin", v);
    } else {
        cJSON_AddStringToObject(b, "level", "unknown");
        cJSON_AddStringToObject(b, "status", "No battery reading. If a battery is connected, send the two pin "
                                              "readings below to the developer.");
    }
    cJSON_Delete(rep);
    char pv[24];
    snprintf(pv, sizeof(pv), "%.0f mV", bat.pin_mv[0]);
    cJSON_AddStringToObject(b, "GPIO1 reading", pv);
    snprintf(pv, sizeof(pv), "%.0f mV", bat.pin_mv[1]);
    cJSON_AddStringToObject(b, "GPIO8 reading", pv);

    // Microphone input levels: talk and these rise on the mic channels;
    // the speaker test makes the reference channel rise.
    float db[4];
    int nch = audio_input_levels(db, 4);
    if (nch > 0) {
        cJSON *mic = add_obj(root, "audio_input");
        const char *fmt = CONFIG_VA_AFE_INPUT_FORMAT;
        for (int i = 0; i < nch; i++) {
            char key[32], val[24];
            char role = fmt[i] ? fmt[i] : '?';
            snprintf(key, sizeof(key), "channel %d (%s)", i + 1,
                     role == 'M' ? "mic" : role == 'R' ? "speaker reference" : "unused");
            snprintf(val, sizeof(val), "%.0f dB", db[i]);
            cJSON_AddStringToObject(mic, key, val);
        }
    }

    // Modules
    cJSON *mods = add_obj(root, "modules");
    portENTER_CRITICAL(&s_status_mux);
    char copy[MAX_STATUS][2][48];
    int n = 0;
    for (int i = 0; i < MAX_STATUS; i++) {
        if (s_status[i].module[0]) {
            strlcpy(copy[n][0], s_status[i].module, sizeof(copy[n][0]));
            strlcpy(copy[n][1], s_status[i].status, sizeof(copy[n][1]));
            n++;
        }
    }
    portEXIT_CRITICAL(&s_status_mux);
    for (int i = 0; i < n; i++) {
        cJSON_AddStringToObject(mods, copy[i][0], copy[i][1]);
    }
    return root;
}

// ---------- Gemini API key check ----------

static cJSON *result(bool ok, int status, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddNumberToObject(r, "http_status", status);
    cJSON_AddStringToObject(r, "message", msg);
    return r;
}

// ---- live voice test: a real Gemini Live session with a one-word prompt

static bool (*s_busy)(void);
void diag_set_busy_check(bool (*busy)(void)) { s_busy = busy; }

static SemaphoreHandle_t s_live_sem;
static volatile bool s_live_ready, s_live_failed, s_live_turn;
static volatile size_t s_live_samples;

static void live_state(gemini_state_t st, void *ctx)
{
    if (st == GEMINI_STATE_READY) {
        s_live_ready = true;
        xSemaphoreGive(s_live_sem);
    } else if (st == GEMINI_STATE_ERROR || st == GEMINI_STATE_CLOSED) {
        s_live_failed = true;
        xSemaphoreGive(s_live_sem);
    }
}

static void live_audio(const int16_t *pcm, size_t n, void *ctx) { s_live_samples += n; }   // not played

static void live_turn(void *ctx)
{
    s_live_turn = true;
    xSemaphoreGive(s_live_sem);
}

/** Returns NULL on success (and fills secs), else a reason (static or `why`). */
static const char *live_test(const char *key, char *why, size_t why_len, float *secs, float *took)
{
    if (s_busy && s_busy()) return "Jarvis is busy (in a conversation, recording or announcing). Try again in a moment.";
    if (!s_live_sem) s_live_sem = xSemaphoreCreateBinary();
    s_live_ready = s_live_failed = s_live_turn = false;
    s_live_samples = 0;
    xSemaphoreTake(s_live_sem, 0);
    gemini_live_config_t cfg = {
        .api_key = key,
        .model = settings_get_model(),
        .voice = settings_get_voice(),
        .system_instruction = "This is a connection test. Reply with the single word: OK.",
        .on_state = live_state,
        .on_audio_out = live_audio,
        .on_turn_complete = live_turn,
    };
    int64_t t0 = esp_timer_get_time();
    if (gemini_live_start(&cfg) != ESP_OK) {
        snprintf(why, why_len, "Couldn't start a voice session: out of internal memory (%u KB free, largest "
                 "block %u KB). Restart the board and try again.",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        return why;
    }
    const char *res = NULL;
    // 1. Connect and set up, as a conversation does.
    while (!s_live_ready && !s_live_failed) {
        if (xSemaphoreTake(s_live_sem, pdMS_TO_TICKS(15000)) != pdTRUE) { res = "No answer from Gemini Live within 15 s."; break; }
    }
    // 2. One short prompt; wait for spoken audio and the end of the turn.
    if (!res && !s_live_failed) {
        if (gemini_live_send_text("Say OK.") != ESP_OK) res = "Couldn't send the test prompt.";
        while (!res && !s_live_turn && !s_live_failed) {
            if (xSemaphoreTake(s_live_sem, pdMS_TO_TICKS(20000)) != pdTRUE) { res = "Gemini Live didn't answer the test prompt within 20 s."; break; }
        }
    }
    if (!res && s_live_failed) {
        const char *err = gemini_live_last_error();
        if (gemini_error_is_account(err)) {
            snprintf(why, why_len, "Google refused because of the account: %s. Check Billing in AI Studio "
                     "(aistudio.google.com).", err);
        } else {
            snprintf(why, why_len, "%s", err[0] ? err : "the connection closed");
        }
        res = why;
    }
    if (!res && s_live_samples == 0) res = "Gemini Live answered, but without any speech.";
    gemini_live_stop();
    *secs = s_live_samples / 24000.0f;
    *took = (esp_timer_get_time() - t0) / 1e6f;
    return res;
}

cJSON *diag_test_gemini(void)
{
    char key[SETTINGS_API_KEY_MAX];
    if (settings_get_api_key(key, sizeof(key)) != ESP_OK) {
        return result(false, 0, "No API key saved. Add one on the setup page.");
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    if (tm.tm_year <= 120) {
        memset(key, 0, sizeof(key));
        return result(false, 0, "Clock not synced yet, so secure connections can't be checked. "
                                "Make sure Wi-Fi is connected and try again in a minute.");
    }

    char url[160];
    char *live_key = strdup(key);           // for step 2; wiped below
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s",
             settings_get_model());
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "x-goog-api-key", key);   // header, never in the URL
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    memset(key, 0, sizeof(key));

    if (err != ESP_OK || status != 200) {
        if (live_key) { memset(live_key, 0, strlen(live_key)); free(live_key); }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gemini check failed: %s", esp_err_to_name(err));
        return result(false, 0, "Couldn't reach Google (network, DNS or TLS problem). "
                                "Check Wi-Fi and try again.");
    }
    ESP_LOGI(TAG, "Gemini check: HTTP %d", status);
    switch (status) {
    case 200: {
        // Step 2: the key and model exist; now try a real voice session (this is what
        // fails when the account is out of credit, or the model doesn't do Live audio).
        char why[220] = "", msg[400];
        float secs = 0, took = 0;
        const char *fail = live_key ? live_test(live_key, why, sizeof(why), &secs, &took) : "Out of memory.";
        if (live_key) { memset(live_key, 0, strlen(live_key)); free(live_key); }
        ESP_LOGI(TAG, "Gemini live test: %s", fail ? fail : "ok");
        if (fail) {
            snprintf(msg, sizeof(msg), "The API key and model \"%s\" are valid, but the live voice test failed: %s",
                     settings_get_model(), fail);
            cJSON *r = result(false, status, msg);
            cJSON_AddStringToObject(r, "step", "live");
            return r;
        }
        snprintf(msg, sizeof(msg), "Works: the key is valid, and the model \"%s\" answered a live voice test "
                 "(%.1f s of speech, %.1f s to answer).", settings_get_model(), secs, took);
        cJSON *r = result(true, status, msg);
        cJSON_AddStringToObject(r, "step", "live");
        return r;
    }
    case 404: {
        char msg[160];
        snprintf(msg, sizeof(msg), "The key works, but Google doesn't know the model \"%s\". "
                 "Check the model name on the setup page.", settings_get_model());
        return result(false, status, msg);
    }
    case 400: return result(false, status, "Google rejected the key. Check it was copied completely.");
    case 401:
    case 403: return result(false, status, "Key not authorized. It may be deleted or restricted to other APIs.");
    case 429: return result(false, status, "Rate limit or quota reached. The key is valid; try later.");
    default:  return result(false, status, "Unexpected response from Google.");
    }
}
