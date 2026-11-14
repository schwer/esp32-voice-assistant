// SPDX-License-Identifier: MIT
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
#include "net.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static volatile bool s_want;          // we want to be online (not powered down for battery)
static int s_attempts;
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
        if (!s_want) {
            return;
        }
        s_attempts++;
        ESP_LOGW(TAG, "disconnected (reason %d), attempt %d", d->reason, s_attempts);
        if (s_attempts < FAST_RETRIES) {
            esp_wifi_connect();                       // transient failures are common
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED); // tell waiters, keep trying slowly
            esp_timer_start_once(s_retry_timer, RETRY_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_attempts = 0;
        ESP_LOGI(TAG, "connected");
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

    if (s_started) {
        esp_wifi_disconnect();
    }
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

esp_err_t net_device_url(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!net_is_connected() || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(buf, len, "http://" IPSTR "/", IP2STR(&ip.ip));
    return ESP_OK;
}
