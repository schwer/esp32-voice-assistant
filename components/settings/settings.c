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
static char s_model[SETTINGS_MODEL_MAX];
static char s_voice[SETTINGS_VOICE_MAX];
static char s_location[SETTINGS_LOCATION_MAX];
static bool s_fahrenheit, s_search = true, s_saver = true;

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
    load(h, "model", s_model, sizeof(s_model), CONFIG_VA_GEMINI_MODEL);
    load(h, "voice", s_voice, sizeof(s_voice), CONFIG_VA_VOICE_NAME);
    load(h, "location", s_location, sizeof(s_location), "");
    // Fahrenheit by default for US time zones, Celsius elsewhere.
    static const char *const US_TZ[] = { "EST", "CST", "MST", "PST", "AKST", "HST" };
    uint8_t f = 0;
    for (size_t i = 0; i < sizeof(US_TZ) / sizeof(US_TZ[0]); i++) {
        if (strncmp(s_tz, US_TZ[i], strlen(US_TZ[i])) == 0) f = 1;
    }
    nvs_get_u8(h, "fahrenheit", &f);
    s_fahrenheit = f != 0;
    uint8_t search = 1;
    nvs_get_u8(h, "search", &search);
    s_search = search != 0;
    uint8_t saver = 1;
    nvs_get_u8(h, "saver", &saver);
    s_saver = saver != 0;
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

const char *settings_get_model(void) { return s_model; }
const char *settings_get_voice(void) { return s_voice; }
esp_err_t settings_set_model(const char *m) { return save("model", m, s_model, sizeof(s_model)); }
esp_err_t settings_set_voice(const char *v) { return save("voice", v, s_voice, sizeof(s_voice)); }

static esp_err_t save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const char *settings_get_location(void) { return s_location; }
esp_err_t settings_set_location(const char *l) { return save("location", l, s_location, sizeof(s_location)); }
bool settings_get_fahrenheit(void) { return s_fahrenheit; }
esp_err_t settings_set_fahrenheit(bool on)
{
    esp_err_t err = save_u8("fahrenheit", on);
    if (err == ESP_OK) s_fahrenheit = on;
    return err;
}
bool settings_get_search(void) { return s_search; }
esp_err_t settings_set_search(bool on)
{
    esp_err_t err = save_u8("search", on);
    if (err == ESP_OK) s_search = on;
    return err;
}

bool settings_get_battery_saver(void) { return s_saver; }
esp_err_t settings_set_battery_saver(bool on)
{
    esp_err_t err = save_u8("saver", on);
    if (err == ESP_OK) s_saver = on;
    return err;
}
