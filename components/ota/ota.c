// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ota.h"
#include "ota_manifest.h"

static const char *TAG = "ota";

#define MANIFEST_MAX   16384
#define CHUNK          4096
#define NVS_NS         "ota"
#define CHECK_EVERY_US (12LL * 3600 * 1000000)

static SemaphoreHandle_t s_lock;
static ota_status_t s_status;
static ota_manifest_t s_manifest;
static volatile bool s_install_requested;
static char s_error[120];
static bool s_trial;
static char s_from[48];

static bool (*s_online)(void);

// ---------------------------------------------------------------- helpers

static void set_error(const char *fmt, const char *arg)
{
    snprintf(s_error, sizeof(s_error), fmt, arg ? arg : "");
    ESP_LOGW(TAG, "%s", s_error);
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[2 * n] = 0;
}

static void nvs_put(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (val) nvs_set_str(h, key, val); else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_get(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

static esp_http_client_handle_t open_url(const char *url, int64_t *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // HTTPS with a checked certificate
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .max_redirection_count = 3,
    };
    if (strncmp(url, "https://", 8) != 0) return NULL;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    *len = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

/** SHA-256 of the first `len` bytes of a partition, as hex. */
static bool partition_sha(const esp_partition_t *p, long len, char *out_hex)
{
    if (!p || len <= 0 || (size_t)len > p->size) return false;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return false;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (long off = 0; off < len && ok; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        ok = esp_partition_read(p, off, buf, n) == ESP_OK;
        if (ok) mbedtls_sha256_update(&ctx, buf, n);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    free(buf);
    if (ok) hex(d, 32, out_hex);
    return ok;
}

// ---------------------------------------------------------------- checking

static esp_err_t check_locked(void)
{
    ota_status_t st = { .checked = true };
    strlcpy(st.current, esp_app_get_description()->version, sizeof(st.current));
    char url[160];
    snprintf(url, sizeof(url), "%s?t=%lld", CONFIG_VA_UPDATE_URL, (long long)(esp_timer_get_time() / 1000));
    int64_t len = 0;
    esp_http_client_handle_t c = open_url(url, &len);
    if (!c) {
        strlcpy(st.why, "couldn't reach the update site", sizeof(st.why));
        s_status = st;
        return ESP_FAIL;
    }
    char *json = calloc(1, MANIFEST_MAX);
    int got = 0, n;
    while (json && got < MANIFEST_MAX - 1 && (n = esp_http_client_read(c, json + got, MANIFEST_MAX - 1 - got)) > 0) {
        got += n;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    bool parsed = json && ota_parse_manifest(json, &s_manifest);
    free(json);
    if (!parsed) {
        strlcpy(st.why, "the update site doesn't describe an update", sizeof(st.why));
        s_status = st;
        return ESP_FAIL;
    }
    st.ok = true;
    strlcpy(st.latest, s_manifest.version, sizeof(st.latest));
    if (ota_is_newer(st.current, st.latest)) {
        const esp_partition_t *model = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
        char sha[65] = "";
        if (s_manifest.layout != OTA_LAYOUT || !esp_ota_get_next_update_partition(NULL)) {
            st.needs_usb = true;
            strlcpy(st.why, "this version changes how the board's memory is laid out", sizeof(st.why));
        } else if (s_manifest.model_sha256[0] &&
                   (!partition_sha(model, s_manifest.model_size, sha) || strcmp(sha, s_manifest.model_sha256))) {
            st.needs_usb = true;
            strlcpy(st.why, "this version has a new \"Jarvis\" listening model", sizeof(st.why));
        } else if (!s_manifest.app_sha256[0] || s_manifest.app_size <= 0) {
            st.needs_usb = true;
            strlcpy(st.why, "the update site doesn't give a checksum for it", sizeof(st.why));
        } else {
            st.available = true;
        }
    }
    s_status = st;
    ESP_LOGI(TAG, "on %s, latest %s: %s", st.current, st.latest,
             st.available ? "update available" : st.needs_usb ? "update needs the web page" : "up to date");
    return ESP_OK;
}

esp_err_t ota_check(ota_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = check_locked();
    if (out) *out = s_status;
    xSemaphoreGive(s_lock);
    return err;
}

ota_status_t ota_last_status(void)
{
    ota_status_t st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_status;
    xSemaphoreGive(s_lock);
    return st;
}

static void check_task(void *arg)
{
    int64_t last = -CHECK_EVERY_US;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        if (esp_timer_get_time() - last < CHECK_EVERY_US || (s_online && !s_online())) continue;
        if (ota_check(NULL) == ESP_OK) last = esp_timer_get_time();
    }
}

void ota_start_background_checks(bool (*online)(void))
{
    s_online = online;
    xTaskCreate(check_task, "ota_check", 6144, NULL, 2, NULL);
}

// ---------------------------------------------------------------- installing

void ota_request_install(void) { s_install_requested = true; }

bool ota_take_install_request(void)
{
    bool r = s_install_requested;
    s_install_requested = false;
    return r;
}

const char *ota_last_error(void) { return s_error; }

/** Is this image our firmware for this chip? Checked on the first bytes before writing. */
static bool image_is_ours(const uint8_t *d, size_t n)
{
    size_t need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (n < need) return false;
    const esp_image_header_t *ih = (const esp_image_header_t *)d;
    const esp_app_desc_t *app = (const esp_app_desc_t *)(d + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    return ih->magic == ESP_IMAGE_HEADER_MAGIC && ih->chip_id == ESP_CHIP_ID_ESP32S3 &&
           app->magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strcmp(app->project_name, esp_app_get_description()->project_name) == 0;
}

static esp_err_t install_locked(void (*progress)(int))
{
    s_error[0] = 0;
    if (check_locked() != ESP_OK) { set_error("%s", s_status.why); return ESP_FAIL; }
    if (!s_status.available) {
        set_error("%s", s_status.needs_usb ? s_status.why : "already on the latest version");
        return ESP_ERR_INVALID_STATE;
    }
    char url[192];
    if (!ota_url_join(CONFIG_VA_UPDATE_URL, s_manifest.app_path, url, sizeof(url))) {
        set_error("bad download path in the manifest%s", NULL);
        return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || s_manifest.app_size > (long)part->size) {
        set_error("the new version doesn't fit%s", NULL);
        return ESP_FAIL;
    }
    int64_t len = 0;
    esp_http_client_handle_t c = open_url(url, &len);
    if (!c) { set_error("couldn't download the update%s", NULL); return ESP_FAIL; }
    if (len > 0 && len != s_manifest.app_size) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        set_error("the download has the wrong size%s", NULL);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloading %s (%ld bytes) into %s", s_manifest.version, s_manifest.app_size, part->label);

    uint8_t *buf = malloc(CHUNK);
    esp_ota_handle_t h = 0;
    esp_err_t err = buf ? esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) : ESP_ERR_NO_MEM;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    long total = 0;
    int last_pct = -1;
    bool begun = err == ESP_OK;
    while (err == ESP_OK) {
        // The first block is collected whole so the header can be checked before writing.
        size_t want = total == 0 ? CHUNK : CHUNK;
        int n = 0, r;
        while (n < (int)want && (r = esp_http_client_read(c, (char *)buf + n, want - n)) > 0) {
            n += r;
            if (total > 0) break;
        }
        if (n <= 0) break;
        if (total == 0 && !image_is_ours(buf, n)) {
            set_error("the download isn't this assistant's firmware%s", NULL);
            err = ESP_ERR_INVALID_VERSION;
            break;
        }
        if (total + n > s_manifest.app_size) { set_error("the download is too big%s", NULL); err = ESP_FAIL; break; }
        mbedtls_sha256_update(&ctx, buf, n);
        err = esp_ota_write(h, buf, n);
        total += n;
        int pct = (int)(total * 100 / s_manifest.app_size);
        if (progress && pct / 5 != last_pct / 5) progress(pct);
        last_pct = pct;
    }
    uint8_t d[32];
    char sha[65];
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    hex(d, 32, sha);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);

    if (err == ESP_OK && total != s_manifest.app_size) { set_error("the download was cut off%s", NULL); err = ESP_FAIL; }
    if (err == ESP_OK && strcmp(sha, s_manifest.app_sha256) != 0) {
        set_error("the download doesn't match its checksum%s", NULL);
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        if (!s_error[0]) set_error("writing the update failed (%s)", esp_err_to_name(err));
        if (begun) esp_ota_abort(h);
        return err;
    }
    err = esp_ota_end(h);                               // checks the image's own hash too
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        set_error("the new version didn't pass its checks (%s)", esp_err_to_name(err));
        return err;
    }
    nvs_put("from", s_status.current);                  // to say "I've been updated" (or not)
    ESP_LOGI(TAG, "update to %s ready; restarting", s_manifest.version);
    return ESP_OK;
}

esp_err_t ota_install(void (*progress)(int percent))
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = install_locked(progress);
    xSemaphoreGive(s_lock);
    return err;
}

// ---------------------------------------------------------------- trial runs

void ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    strlcpy(s_status.current, esp_app_get_description()->version, sizeof(s_status.current));
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_trial = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    nvs_get("from", s_from, sizeof(s_from));
    if (s_trial) ESP_LOGW(TAG, "new version %s on trial (updated from %s)", s_status.current, s_from);
}

bool ota_on_trial(void) { return s_trial; }

const char *ota_confirm(void)
{
    static char from[48];
    if (s_trial) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_trial = false;
        ESP_LOGI(TAG, "version %s confirmed", s_status.current);
    }
    strlcpy(from, s_from, sizeof(from));
    if (s_from[0] && strcmp(s_from, s_status.current) != 0) {
        nvs_put("from", NULL);
        s_from[0] = 0;
        return from;
    }
    return "";
}

void ota_roll_back(void)
{
    ESP_LOGE(TAG, "version %s didn't work out: going back to the previous one", s_status.current);
    nvs_put("rolled", "1");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool ota_take_rolled_back(void)
{
    char v[4];
    bool rolled = nvs_get("rolled", v, sizeof(v));
    // Also: we asked for an update, but we're still on the version we updated from
    // (the new one failed its boot checks and the bootloader went back).
    bool same = s_from[0] && strcmp(s_from, s_status.current) == 0;
    if (rolled || same) {
        nvs_put("rolled", NULL);
        nvs_put("from", NULL);
        s_from[0] = 0;
        return true;
    }
    return false;
}

bool ota_should_mention(char *version, size_t len)
{
    ota_status_t st = ota_last_status();
    char told[48];
    if (!st.available) return false;
    if (nvs_get("told", told, sizeof(told)) && strcmp(told, st.latest) == 0) return false;
    nvs_put("told", st.latest);                         // once per new version
    strlcpy(version, st.latest, len);
    return true;
}

// ---------------------------------------------------------------- tool

cJSON *ota_tool_check(void)
{
    ota_status_t st;
    ota_check(&st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "current_version", st.current);
    if (!st.ok) {
        cJSON_AddStringToObject(r, "error", st.why);
        return r;
    }
    cJSON_AddStringToObject(r, "latest_version", st.latest);
    cJSON_AddBoolToObject(r, "update_available", st.available);
    if (st.needs_usb) {
        cJSON_AddStringToObject(r, "needs_install_from_web_page", st.why);
    }
    return r;
}
