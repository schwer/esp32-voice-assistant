// SPDX-License-Identifier: MIT
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "settings";
#define NS "va"

static char s_tz[SETTINGS_TZ_MAX];
static char s_name[SETTINGS_NAME_MAX];
static char s_key[SETTINGS_API_KEY_MAX];
static bool s_spoken = true;

static void load(nvs_handle_t h, const char *key, char *buf, size_t len, const char *fallback)
{
    size_t n = len;
    if (nvs_get_str(h, key, buf, &n) != ESP_OK) {
        strlcpy(buf, fallback, len);
    }
}

esp_err_t settings_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    load(h, "tz", s_tz, sizeof(s_tz), CONFIG_VA_TIMEZONE);
    load(h, "name", s_name, sizeof(s_name), "Jarvis");
    load(h, "api_key", s_key, sizeof(s_key), CONFIG_VA_GEMINI_API_KEY);
    uint8_t spoken = 1;
    nvs_get_u8(h, "spoken", &spoken);
    s_spoken = spoken != 0;
    nvs_close(h);
    ESP_LOGI(TAG, "tz=%s name=%s api_key=%s", s_tz, s_name, s_key[0] ? "set" : "missing");
    return ESP_OK;
}

static esp_err_t save(const char *key, const char *value, char *cache, size_t len)
{
    if (strlen(value) >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(cache, value, len);
    }
    return err;
}

const char *settings_get_tz(void)   { return s_tz; }
const char *settings_get_name(void) { return s_name; }
bool settings_has_api_key(void)     { return s_key[0] != 0; }

esp_err_t settings_get_api_key(char *buf, size_t len)
{
    if (!s_key[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(buf, s_key, len);
    return ESP_OK;
}

void settings_api_key_hint(char *buf, size_t len)
{
    size_t n = strlen(s_key);
    strlcpy(buf, n >= 4 ? s_key + n - 4 : "", len);
}

esp_err_t settings_set_tz(const char *tz)     { return save("tz", tz, s_tz, sizeof(s_tz)); }
esp_err_t settings_set_name(const char *name) { return save("name", name, s_name, sizeof(s_name)); }
esp_err_t settings_set_api_key(const char *k) { return save("api_key", k, s_key, sizeof(s_key)); }

bool settings_get_spoken_status(void) { return s_spoken; }

esp_err_t settings_set_spoken_status(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "spoken", on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_spoken = on;
    return err;
}
