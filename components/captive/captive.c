// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "net.h"
#include "web_util.h"
#include "captive.h"
#include "captive_page.h"

static const char *TAG = "captive";
#define AP_IP_BYTES 192, 168, 4, 1           // esp_netif's default for the hotspot

static httpd_handle_t s_server;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static int s_dns_sock = -1;

// ---------------------------------------------------------------- DNS: every name -> us

static void dns_task(void *arg)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_sock < 0 || bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        if (s_dns_sock >= 0) close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    static uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        // Header: answer the (single) question with our address, for A queries.
        int qd = (buf[4] << 8) | buf[5];
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;          // skip the name
        if (qd != 1 || p + 5 > n) continue;
        int qtype = (buf[p + 1] << 8) | buf[p + 2];
        int end = p + 5;
        buf[2] = 0x81;                                     // response, recursion desired
        buf[3] = 0x80;                                     // recursion available, no error
        buf[6] = 0; buf[7] = qtype == 1 ? 1 : 0;           // answers
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, AP_IP_BYTES };
            memcpy(buf + end, ans, sizeof(ans));
            end += sizeof(ans);
        }
        sendto(s_dns_sock, buf, end, 0, (struct sockaddr *)&from, flen);
    }
    close(s_dns_sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- web

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, CAPTIVE_PAGE, sizeof(CAPTIVE_PAGE) - 1);
}

/** Anything else (phones' "is there internet?" checks included): send them to the page. */
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_scan(httpd_req_t *req)
{
    wifi_ap_record_t *aps = heap_caps_calloc(20, sizeof(*aps), MALLOC_CAP_SPIRAM);
    int n = aps ? net_scan(aps, 20) : 0;
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    return send_json(req, r);
}

// Joining runs in its own task: the hotspot may change channel while the board joins.
static char s_ssid[NET_SSID_MAX], s_pass[NET_PASS_MAX];
static volatile int s_state;                 // 0 idle, 1 connecting, 2 connected, 3 failed

static void join_task(void *arg)
{
    esp_err_t err = net_provision(s_ssid, s_pass, 20000);   // saves it (encrypted) if it works
    memset(s_pass, 0, sizeof(s_pass));
    s_state = err == ESP_OK ? 2 : 3;
    ESP_LOGI(TAG, "joining '%s': %s", s_ssid, err == ESP_OK ? "connected" : "failed");
    vTaskDelete(NULL);
}

static esp_err_t post_connect(httpd_req_t *req)
{
    char body[256] = "";
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1, got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    cJSON *res = cJSON_CreateObject();
    char ssid[NET_SSID_MAX * 3] = "", pass[NET_PASS_MAX * 3] = "";
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) == ESP_OK) {
        web_url_decode(ssid);
        web_trim(ssid);
    }
    if (httpd_query_key_value(body, "pass", pass, sizeof(pass)) == ESP_OK) web_url_decode(pass);   // spaces count
    memset(body, 0, sizeof(body));
    size_t pl = strlen(pass);
    if (s_state == 1) {
        cJSON_AddBoolToObject(res, "ok", false);
        cJSON_AddStringToObject(res, "message", "Still trying the last network. Wait a moment.");
    } else if (!ssid[0] || strlen(ssid) > 32 || (pl && (pl < 8 || pl > 63))) {
        cJSON_AddBoolToObject(res, "ok", false);
        cJSON_AddStringToObject(res, "message", "Check the network name, and that the password is 8 to 63 characters (or blank).");
    } else {
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_pass, pass, sizeof(s_pass));
        s_state = 1;
        if (xTaskCreate(join_task, "wifi_join", 4096, NULL, 4, NULL) != pdPASS) s_state = 3;
        cJSON_AddBoolToObject(res, "ok", s_state == 1);
        if (s_state != 1) cJSON_AddStringToObject(res, "message", "Out of memory. Restart Jarvis and try again.");
    }
    memset(pass, 0, sizeof(pass));
    return send_json(req, res);
}

static esp_err_t get_status(httpd_req_t *req)
{
    static const char *const names[] = { "idle", "connecting", "connected", "failed" };
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", names[s_state]);
    cJSON_AddStringToObject(r, "ssid", s_ssid);
    if (s_state == 2) {
        char url[64] = "";
        if (net_device_url(url, sizeof(url)) == ESP_OK) cJSON_AddStringToObject(r, "url", url);
    } else if (s_state == 3) {
        cJSON_AddStringToObject(r, "message", "Couldn't join that network. Check the password, and that it's a "
                                              "2.4 GHz network in range, then try again.");
    }
    return send_json(req, r);
}

esp_err_t captive_start(void)
{
    if (s_server) return ESP_OK;
    s_state = 0;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 4096;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 6;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;
    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = get_page },
        { .uri = "/scan.json",   .method = HTTP_GET,  .handler = get_scan },
        { .uri = "/connect",     .method = HTTP_POST, .handler = post_connect },
        { .uri = "/status.json", .method = HTTP_GET,  .handler = get_status },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect);
    s_dns_run = true;
    // DNS stack in PSRAM: it never writes flash.
    if (xTaskCreateWithCaps(dns_task, "captive_dns", 3072, NULL, 4, &s_dns_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "DNS responder didn't start: open http://192.168.4.1/ by hand");
    }
    ESP_LOGI(TAG, "Wi-Fi setup page on the hotspot: http://192.168.4.1/");
    return ESP_OK;
}

void captive_stop(void)
{
    s_dns_run = false;                                      // the DNS task ends within a second
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
