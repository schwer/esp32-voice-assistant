// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
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
#include "storage.h"
#include "conversations.h"
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
static struct { char module[16]; char status[48]; } s_status[MAX_STATUS];
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
    cJSON_AddNumberToObject(mem, "internal_free_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "internal_min_free_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "internal_largest_block_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(mem, "psram_total_kb", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(mem, "psram_free_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    cJSON_AddNumberToObject(mem, "psram_min_free_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);

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
    }
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

    esp_http_client_config_t cfg = {
        .url = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=1",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "x-goog-api-key", key);   // header, never in the URL
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    memset(key, 0, sizeof(key));

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gemini check failed: %s", esp_err_to_name(err));
        return result(false, 0, "Couldn't reach Google (network, DNS or TLS problem). "
                                "Check Wi-Fi and try again.");
    }
    ESP_LOGI(TAG, "Gemini check: HTTP %d", status);
    switch (status) {
    case 200: return result(true, status, "API key works.");
    case 400: return result(false, status, "Google rejected the key. Check it was copied completely.");
    case 401:
    case 403: return result(false, status, "Key not authorized. It may be deleted or restricted to other APIs.");
    case 429: return result(false, status, "Rate limit or quota reached. The key is valid; try later.");
    default:  return result(false, status, "Unexpected response from Google.");
    }
}
