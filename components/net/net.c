// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static volatile bool s_want;          // we want to be online (not powered down for battery)
static int s_attempts;
static volatile int s_last_reason;     // last disconnect reason (0 = none yet)
static esp_timer_handle_t s_retry_timer;

#define FAST_RETRIES   5              // quick retries before reporting failure
#define RETRY_DELAY_US (10 * 1000 * 1000)

static void retry_cb(void *arg)
{
    if (s_want && s_started) {
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        s_last_reason = d->reason;
        if (!s_want) {
            return;
        }
        s_attempts++;
        ESP_LOGW(TAG, "disconnected: %s (reason %d), attempt %d",
                 net_reason_text(d->reason), d->reason, s_attempts);
        if (s_attempts < FAST_RETRIES) {
            esp_wifi_connect();                       // transient failures are common
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED); // tell waiters, keep trying slowly
            esp_timer_start_once(s_retry_timer, RETRY_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_attempts = 0;
        s_last_reason = 0;
        ESP_LOGI(TAG, "connected, IP " IPSTR " - setup page: http://" IPSTR "/",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
        xEventGroupClearBits(s_events, BIT_FAILED);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        static bool sntp_started;
        if (!sntp_started) {
            // Clock for TLS, Gemini's sense of "today", and reminders.
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started = esp_netif_sntp_init(&c) == ESP_OK;
        }
    }
}

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "assistant");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    // Networks saved by older firmware: prefer the strongest access point too.
    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0] &&
        (saved.sta.scan_method != WIFI_ALL_CHANNEL_SCAN || saved.sta.sort_method != WIFI_CONNECT_AP_BY_SIGNAL ||
         saved.sta.listen_interval != 3)) {
        saved.sta.listen_interval = 3;
        saved.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        saved.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        esp_wifi_set_config(WIFI_IF_STA, &saved);
    }
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    return ESP_OK;
}

bool net_has_credentials(void)
{
    wifi_config_t c = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0] != 0;
}

static esp_err_t start_and_wait(int timeout_ms)
{
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_want = true;
    s_attempts = 0;
    esp_timer_stop(s_retry_timer);
    if (!s_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_started = true;
    }
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) ? ESP_OK : ESP_FAIL;
}

esp_err_t net_connect(int timeout_ms)
{
    if (net_is_connected()) {
        return ESP_OK;
    }
    if (!net_has_credentials()) {
        return ESP_ERR_NOT_FOUND;
    }
    return start_and_wait(timeout_ms);
}

esp_err_t net_provision(const char *ssid, const char *password, int timeout_ms)
{
    wifi_config_t old = {0};
    esp_wifi_get_config(WIFI_IF_STA, &old);

    wifi_config_t c = {0};
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, password, sizeof(c.sta.password));
    c.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    c.sta.listen_interval = 3;                        // doze up to 3 beacons when idle
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;        // several access points can share a name:
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;    // join the strongest, not the first found
    c.sta.pmf_cfg.capable = true;                    // allow WPA2/WPA3 mixed networks
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;            // and WPA3-only networks

    s_want = false;                                   // don't auto-retry the old network
    esp_timer_stop(s_retry_timer);
    if (s_started) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));               // let the disconnect event drain
    }
    ESP_LOGI(TAG, "trying new Wi-Fi network '%s'", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &c));
    esp_err_t err = start_and_wait(timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not join '%s', restoring previous settings", ssid);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &old);
        if (old.sta.ssid[0]) {
            s_attempts = 0;
            esp_wifi_connect();                       // back to the previous network
        }
    }
    return err;
}

void net_disconnect(void)
{
    s_want = false;
    esp_timer_stop(s_retry_timer);
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
    xEventGroupClearBits(s_events, BIT_CONNECTED);
}

bool net_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

const char *net_reason_text(int reason)
{
    switch (reason) {
    case 0:   return "no attempt yet";
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found (check the name; it must be a 2.4 GHz network)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong Wi-Fi password";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_BEACON_TIMEOUT:
        return "weak signal or router not responding";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "router has too many devices connected";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    default:
        return "connection lost";
    }
}

void net_log_status(void)
{
    if (net_is_connected()) {
        char url[64] = "";
        net_device_url(url, sizeof(url));
        ESP_LOGI(TAG, "STATUS: Wi-Fi connected. Setup page: %s", url);
        return;
    }
    wifi_config_t c = {0};
    esp_wifi_get_config(WIFI_IF_STA, &c);
    if (!c.sta.ssid[0]) {
        ESP_LOGW(TAG, "STATUS: no Wi-Fi saved. Use 'Configure Wi-Fi' on the install page.");
    } else {
        ESP_LOGW(TAG, "STATUS: not connected to '%s': %s. Still retrying.",
                 (const char *)c.sta.ssid, net_reason_text(s_last_reason));
    }
}

esp_err_t net_device_url(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!net_is_connected() || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(buf, len, "http://" IPSTR "/", IP2STR(&ip.ip));
    return ESP_OK;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int net_scan(wifi_ap_record_t *out, int max)
{
    // A scan can't run while the driver is busy retrying a connection.
    bool was_wanted = s_want;
    bool connected = net_is_connected();
    if (!connected) {
        s_want = false;
        esp_timer_stop(s_retry_timer);
        if (s_started) esp_wifi_disconnect();
    }
    if (!s_started) {
        esp_wifi_start();
        s_started = true;
    }
    wifi_scan_config_t cfg = { .show_hidden = false };
    int n = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t count = 64;
        wifi_ap_record_t *all = calloc(count, sizeof(*all));
        if (all && esp_wifi_scan_get_ap_records(&count, all) == ESP_OK) {
            qsort(all, count, sizeof(*all), cmp_rssi);
            for (int i = 0; i < count && n < max; i++) {
                if (!all[i].ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < n && !dup; j++) {
                    dup = strcmp((char *)out[j].ssid, (char *)all[i].ssid) == 0;
                }
                if (!dup) out[n++] = all[i];
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(all);
    }
    if (!connected && was_wanted && net_has_credentials()) {
        s_want = true;
        s_attempts = 0;
        esp_wifi_connect();
    }
    return n;
}

void net_current_ssid(char *buf, size_t len)
{
    wifi_ap_record_t ap;
    if (net_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(buf, (const char *)ap.ssid, len);
    } else {
        buf[0] = 0;
    }
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return (net_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

void net_saved_ssid(char *buf, size_t len)
{
    wifi_config_t c = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {
        strlcpy(buf, (const char *)c.sta.ssid, len);
    } else {
        buf[0] = 0;
    }
}

void net_power_save(bool idle)
{
    if (s_started) esp_wifi_set_ps(idle ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
}
