// SPDX-License-Identifier: MIT
//
// Wi-Fi: up to NET_MAX_SAVED networks of the user's (home, phone hotspot, ...), plus,
// if allowed, open networks with no password. Open networks are only used if they
// really reach the internet (not a "sign in to continue" page), and while on one the
// setup page is switched off (net_on_trust_change) so strangers can't change settings.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "net.h"
#include "net_logic.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0      // usable connection
#define BIT_FAILED    BIT1      // current attempt failed
#define BIT_GOT_IP    BIT2      // an attempt got an address (not yet checked)
#define BIT_ABORT     BIT3      // Wi-Fi switched off: stop trying

#define FAST_RETRIES      4             // same network, before looking for another
#define ATTEMPT_MS        15000
#define ROAM_PAUSE_MS     30000         // between searches when nothing worked
#define BLOCK_US          (30LL * 60 * 1000000)   // skip a sign-in-page network for 30 min
#define MAX_BLOCKED       8
#define NVS_NS            "wifi_list"

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static volatile bool s_want;            // we want to be online (not powered down for battery)
static volatile bool s_roaming;         // the roam task is driving connections
static int s_attempts;
static volatile int s_last_reason;      // last disconnect reason (0 = none yet)
static SemaphoreHandle_t s_radio;       // one scan/connection sequence at a time
static TaskHandle_t s_roam_task;

static net_saved_t s_saved[NET_MAX_SAVED];
static int s_n_saved;
static volatile bool s_allow_open = true;
static volatile bool s_untrusted;       // on an open network that isn't one of ours
static net_trust_cb_t s_trust_cb;
static struct { char ssid[NET_SSID_MAX]; int64_t until; } s_blocked[MAX_BLOCKED];

// ---------------------------------------------------------------- saved networks

static void save_list(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "n", (uint8_t)s_n_saved);
    for (int i = 0; i < NET_MAX_SAVED; i++) {
        char ks[4], kp[4];
        snprintf(ks, sizeof(ks), "s%d", i);
        snprintf(kp, sizeof(kp), "p%d", i);
        if (i < s_n_saved) {
            nvs_set_str(h, ks, s_saved[i].ssid);
            nvs_set_str(h, kp, s_saved[i].pass);
        } else {
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_list(void)
{
    nvs_handle_t h;
    s_n_saved = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    for (int i = 0; i < n && i < NET_MAX_SAVED; i++) {
        char ks[4], kp[4];
        snprintf(ks, sizeof(ks), "s%d", i);
        snprintf(kp, sizeof(kp), "p%d", i);
        size_t ls = sizeof(s_saved[0].ssid), lp = sizeof(s_saved[0].pass);
        if (nvs_get_str(h, ks, s_saved[s_n_saved].ssid, &ls) != ESP_OK) continue;
        if (nvs_get_str(h, kp, s_saved[s_n_saved].pass, &lp) != ESP_OK) s_saved[s_n_saved].pass[0] = 0;
        s_n_saved++;
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- events

static void kick_roam(void)
{
    if (s_roam_task) xTaskNotifyGive(s_roam_task);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
        s_last_reason = d->reason;
        if (!s_want) return;
        if (s_roaming) {                              // the roam task decides what's next
            xEventGroupSetBits(s_events, BIT_FAILED);
            return;
        }
        s_attempts++;
        ESP_LOGW(TAG, "disconnected: %s (reason %d), attempt %d",
                 net_reason_text(d->reason), d->reason, s_attempts);
        bool gone = d->reason == WIFI_REASON_NO_AP_FOUND && s_attempts >= 2;
        if (s_attempts < FAST_RETRIES && !gone) {
            esp_wifi_connect();                       // transient failures are common
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
            kick_roam();                              // look for another network
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_attempts = 0;
        s_last_reason = 0;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        static bool sntp_started;
        if (!sntp_started) {
            // Clock for TLS, Gemini's sense of "today", and alarms.
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started = esp_netif_sntp_init(&c) == ESP_OK;
        }
        if (s_roaming) {
            xEventGroupSetBits(s_events, BIT_GOT_IP);  // checked by the roam task first
        } else {
            xEventGroupClearBits(s_events, BIT_FAILED);
            xEventGroupSetBits(s_events, BIT_CONNECTED);
            ESP_LOGI(TAG, "connected - setup page: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        }
    }
}

// ---------------------------------------------------------------- connecting

static void ensure_started(void)
{
    if (!s_started) {
        esp_wifi_start();
        s_started = true;
    }
}

/** Does this network reach the internet, or is it a "sign in to continue" page? */
static bool internet_ok(void)
{
    esp_http_client_config_t cfg = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .timeout_ms = 6000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err == ESP_OK && status == 204;
}

static void block(const char *ssid)
{
    int slot = 0;
    for (int i = 0; i < MAX_BLOCKED; i++) {
        if (s_blocked[i].until < s_blocked[slot].until) slot = i;   // reuse the oldest
    }
    strlcpy(s_blocked[slot].ssid, ssid, sizeof(s_blocked[slot].ssid));
    s_blocked[slot].until = esp_timer_get_time() + BLOCK_US;
}

static void set_trust(bool untrusted)
{
    if (untrusted == s_untrusted) return;
    s_untrusted = untrusted;
    ESP_LOGI(TAG, "%s", untrusted ? "public network: switching the setup page off"
                                  : "own network: setup page on");
    if (s_trust_cb) s_trust_cb(untrusted);
}

/** Try one network. Caller holds s_radio and has set s_roaming. */
static bool try_choice(const net_choice_t *c, bool check_internet)
{
    if (!s_want) return false;
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, c->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, c->pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = c->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.listen_interval = 3;                      // doze up to 3 beacons when idle
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;      // several access points can share a name:
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;  // join the strongest, not the first found
    cfg.sta.pmf_cfg.capable = true;                   // allow WPA2/WPA3 mixed networks
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;          // and WPA3-only networks

    bool untrusted = c->open && !c->saved;
    if (untrusted) set_trust(true);                   // page off before we join
    ensure_started();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));                   // let the disconnect event drain
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED | BIT_GOT_IP);
    ESP_LOGI(TAG, "trying %s network '%s'", c->saved ? "saved" : "open", c->ssid);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED | BIT_ABORT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(ATTEMPT_MS));
    if (!(bits & BIT_GOT_IP) || !s_want) {
        ESP_LOGW(TAG, "couldn't join '%s': %s", c->ssid, net_reason_text(s_last_reason));
        esp_wifi_disconnect();
        return false;
    }
    if (check_internet && untrusted && !internet_ok()) {
        ESP_LOGW(TAG, "'%s' needs a sign-in page or has no internet: skipping it for 30 minutes", c->ssid);
        block(c->ssid);
        esp_wifi_disconnect();
        return false;
    }
    if (!untrusted) set_trust(false);
    s_attempts = 0;
    xEventGroupClearBits(s_events, BIT_FAILED);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
    char url[64] = "";
    net_device_url(url, sizeof(url));
    ESP_LOGI(TAG, "connected to '%s'%s - %s", c->ssid, untrusted ? " (public network)" : "",
             untrusted ? "setup page off here" : url);
    return true;
}

static int do_scan(wifi_ap_record_t *all, uint16_t cap)
{
    ensure_started();
    wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return 0;
    uint16_t count = cap;
    if (esp_wifi_scan_get_ap_records(&count, all) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    return count;
}

/** Look around and join the best network we can. Returns true when connected. */
static bool roam_once(void)
{
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool ok = false;
    s_roaming = true;
    esp_wifi_disconnect();                            // stop the driver's own retries
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_ap_record_t *all = calloc(40, sizeof(*all));
    net_ap_t *aps = calloc(40, sizeof(*aps));
    net_choice_t *choices = calloc(8, sizeof(*choices));
    if (all && aps && choices && s_want) {
        int n = do_scan(all, 40);
        for (int i = 0; i < n; i++) {
            strlcpy(aps[i].ssid, (const char *)all[i].ssid, sizeof(aps[i].ssid));
            aps[i].rssi = all[i].rssi;
            aps[i].open = all[i].authmode == WIFI_AUTH_OPEN;
        }
        char blocked[MAX_BLOCKED][NET_SSID_MAX];
        int nb = 0;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < MAX_BLOCKED; i++) {
            if (s_blocked[i].until > now) strlcpy(blocked[nb++], s_blocked[i].ssid, NET_SSID_MAX);
        }
        int nc = net_pick(aps, n, s_saved, s_n_saved, s_allow_open, (const char (*)[NET_SSID_MAX])blocked,
                          nb, choices, 8);
        ESP_LOGI(TAG, "%d networks nearby, %d worth trying", n, nc);
        for (int i = 0; i < nc && !ok && s_want; i++) ok = try_choice(&choices[i], true);
    }
    free(all);
    free(aps);
    free(choices);
    s_roaming = false;
    if (!ok && s_want) {
        xEventGroupSetBits(s_events, BIT_FAILED);
        if (s_n_saved) {                              // let the driver keep trying our first network
            wifi_config_t c = {0};
            esp_wifi_get_config(WIFI_IF_STA, &c);
            if (strcmp((const char *)c.sta.ssid, s_saved[0].ssid) != 0) {
                net_choice_t first = { .saved = true, .open = !s_saved[0].pass[0] };
                strlcpy(first.ssid, s_saved[0].ssid, sizeof(first.ssid));
                strlcpy(first.pass, s_saved[0].pass, sizeof(first.pass));
                memset(&c, 0, sizeof(c));
                strlcpy((char *)c.sta.ssid, first.ssid, sizeof(c.sta.ssid));
                strlcpy((char *)c.sta.password, first.pass, sizeof(c.sta.password));
                c.sta.threshold.authmode = first.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
                c.sta.pmf_cfg.capable = true;
                c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
                esp_wifi_set_config(WIFI_IF_STA, &c);
            }
        }
    }
    xSemaphoreGive(s_radio);
    return ok;
}

static void roam_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ROAM_PAUSE_MS));
        if (!s_want || net_is_connected() || !s_started) continue;
        if (!s_n_saved && !s_allow_open) continue;
        roam_once();
    }
}

// ---------------------------------------------------------------- public API

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    s_radio = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "assistant");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    // Our own list of networks. Older firmware kept one network in the Wi-Fi driver's
    // settings: adopt it. From now on the driver's settings are working state only.
    load_list();
    wifi_config_t drv = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &drv) == ESP_OK && drv.sta.ssid[0] && !s_n_saved) {
        s_n_saved = net_saved_add(s_saved, s_n_saved, (const char *)drv.sta.ssid, (const char *)drv.sta.password);
        save_list();
        ESP_LOGI(TAG, "kept the saved network '%s'", s_saved[0].ssid);
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (s_n_saved) {                                  // first try: the most recent network
        memset(&drv, 0, sizeof(drv));
        strlcpy((char *)drv.sta.ssid, s_saved[0].ssid, sizeof(drv.sta.ssid));
        strlcpy((char *)drv.sta.password, s_saved[0].pass, sizeof(drv.sta.password));
        drv.sta.threshold.authmode = s_saved[0].pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        drv.sta.listen_interval = 3;
        drv.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        drv.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        drv.sta.pmf_cfg.capable = true;
        drv.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        esp_wifi_set_config(WIFI_IF_STA, &drv);
    }
    // Stack in PSRAM: internal RAM is scarce, and this task never writes flash
    // (the saved-network list is written by the callers of net_provision/add/forget).
    if (xTaskCreateWithCaps(roam_task, "wifi_roam", 4096, NULL, 4, &s_roam_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        xTaskCreate(roam_task, "wifi_roam", 4096, NULL, 4, &s_roam_task);   // fallback (no PSRAM)
    }
    return ESP_OK;
}

bool net_has_credentials(void)
{
    return s_n_saved > 0;
}

esp_err_t net_connect(int timeout_ms)
{
    if (net_is_connected()) {
        return ESP_OK;
    }
    if (!s_n_saved) {
        return ESP_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(s_events, BIT_ABORT | BIT_FAILED);
    bool was_wanted = s_want;
    s_want = true;
    if (!s_started || !was_wanted) {
        ensure_started();
        if (!s_roaming) {
            s_attempts = 0;
            esp_wifi_connect();                       // the last network first; roams if it's gone
        }
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) ? ESP_OK : ESP_FAIL;
}

esp_err_t net_provision(const char *ssid, const char *password, int timeout_ms)
{
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_want = true;
    s_roaming = true;
    net_choice_t c = { .saved = true, .open = !password[0] };
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    strlcpy(c.pass, password, sizeof(c.pass));
    bool ok = try_choice(&c, false);                  // the user chose it: no internet check
    s_roaming = false;
    if (ok) {
        s_n_saved = net_saved_add(s_saved, s_n_saved, ssid, password);
        save_list();
        ESP_LOGI(TAG, "saved '%s' (%d network%s saved)", ssid, s_n_saved, s_n_saved == 1 ? "" : "s");
    } else {
        ESP_LOGW(TAG, "could not join '%s'; keeping the saved networks", ssid);
    }
    xSemaphoreGive(s_radio);
    if (!ok) kick_roam();                             // back to a network that works
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t net_add_network(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 63 ||
        (password[0] && strlen(password) < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_n_saved = net_saved_add(s_saved, s_n_saved, ssid, password);
    save_list();
    ESP_LOGI(TAG, "added '%s' (%d network%s saved)", ssid, s_n_saved, s_n_saved == 1 ? "" : "s");
    if (s_want && !net_is_connected()) kick_roam();
    return ESP_OK;
}

esp_err_t net_forget(const char *ssid)
{
    int n = net_saved_remove(s_saved, s_n_saved, ssid);
    if (n == s_n_saved) return ESP_ERR_NOT_FOUND;
    s_n_saved = n;
    save_list();
    ESP_LOGI(TAG, "forgot '%s'", ssid);
    return ESP_OK;
}

int net_saved_networks(char (*ssids)[NET_SSID_MAX], int max)
{
    int n = s_n_saved < max ? s_n_saved : max;
    for (int i = 0; i < n; i++) strlcpy(ssids[i], s_saved[i].ssid, NET_SSID_MAX);
    return n;
}

void net_set_allow_open(bool allow)
{
    s_allow_open = allow;
    if (!allow && s_untrusted && net_is_connected()) {
        ESP_LOGI(TAG, "open networks turned off: leaving this one");
        esp_wifi_disconnect();                        // the driver retries, then roams to ours
    }
}

bool net_on_public_network(void) { return s_untrusted && net_is_connected(); }

void net_on_trust_change(net_trust_cb_t cb) { s_trust_cb = cb; }

void net_disconnect(void)
{
    s_want = false;
    xEventGroupSetBits(s_events, BIT_ABORT);
    xSemaphoreTake(s_radio, pdMS_TO_TICKS(ATTEMPT_MS + 5000));   // let a search wind down
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
    xSemaphoreGive(s_radio);
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
    if (!s_n_saved) {
        ESP_LOGW(TAG, "STATUS: no Wi-Fi saved. Use 'Configure Wi-Fi' on the install page.");
    } else {
        wifi_config_t c = {0};
        esp_wifi_get_config(WIFI_IF_STA, &c);
        ESP_LOGW(TAG, "STATUS: not connected (last tried '%s': %s). Still looking for a network.",
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
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(20000)) != pdTRUE) return 0;
    // A scan can't run while the driver is busy retrying a connection.
    bool connected = net_is_connected();
    bool was_wanted = s_want;
    if (!connected) {
        s_want = false;
        if (s_started) esp_wifi_disconnect();
    }
    int n = 0;
    uint16_t cap = 64;
    wifi_ap_record_t *all = calloc(cap, sizeof(*all));
    if (all) {
        int count = do_scan(all, cap);
        qsort(all, count, sizeof(*all), cmp_rssi);
        for (int i = 0; i < count && n < max; i++) {
            if (!all[i].ssid[0]) continue;
            bool dup = false;
            for (int j = 0; j < n && !dup; j++) {
                dup = strcmp((char *)out[j].ssid, (char *)all[i].ssid) == 0;
            }
            if (!dup) out[n++] = all[i];
        }
    }
    free(all);
    if (!connected && was_wanted) {
        s_want = true;
        kick_roam();
    }
    xSemaphoreGive(s_radio);
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
    strlcpy(buf, s_n_saved ? s_saved[0].ssid : "", len);
}

void net_power_save(bool idle)
{
    if (s_started) esp_wifi_set_ps(idle ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
}

int net_sockets_in_use(int *max)
{
    int n = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        if (fcntl(fd, F_GETFL, 0) >= 0) n++;
    }
    if (max) *max = CONFIG_LWIP_MAX_SOCKETS;
    return n;
}
