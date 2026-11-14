// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "weather.h"

static const char *TAG = "weather";
#define MAX_BODY (16 * 1024)

typedef struct { char *buf; int len; } body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b->len + e->data_len < MAX_BODY) {
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

static cJSON *get_json(const char *url)
{
    body_t b = { .buf = malloc(MAX_BODY), .len = 0 };
    if (!b.buf) return NULL;
    b.buf[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .event_handler = on_http,
        .user_data = &b,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    cJSON *json = (err == ESP_OK && status == 200) ? cJSON_Parse(b.buf) : NULL;
    if (!json) ESP_LOGW(TAG, "request failed: %s, HTTP %d", esp_err_to_name(err), status);
    free(b.buf);
    return json;
}

static void url_encode(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/** WMO weather interpretation codes, as used by Open-Meteo. */
static const char *describe(int code)
{
    switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: case 81: return "rain showers";
    case 82: return "violent rain showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm with hail";
    default: return "unknown";
    }
}

static cJSON *error(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return r;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    }
    return true;
}

/**
 * Find coordinates. Open-Meteo's geocoder matches place names, so "Boston, MA" is
 * searched as "Boston" and the part after the comma picks among the matches.
 */
static bool geocode(const char *location, double *lat, double *lon, char *label, size_t label_len)
{
    char name[64], hint[48] = "", q[160], url[256];
    strlcpy(name, location, sizeof(name));
    char *comma = strchr(name, ',');
    if (comma) {
        *comma = 0;
        const char *h = comma + 1;
        while (*h == ' ') h++;
        strlcpy(hint, h, sizeof(hint));
    }
    url_encode(name, q, sizeof(q));
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=en&format=json", q);
    cJSON *geo = get_json(url);
    cJSON *results = cJSON_GetObjectItem(geo, "results");
    cJSON *best = cJSON_GetArrayItem(results, 0), *r;
    if (hint[0]) {
        cJSON_ArrayForEach(r, results) {
            const char *admin = cJSON_GetStringValue(cJSON_GetObjectItem(r, "admin1"));
            const char *country = cJSON_GetStringValue(cJSON_GetObjectItem(r, "country"));
            const char *cc = cJSON_GetStringValue(cJSON_GetObjectItem(r, "country_code"));
            if ((admin && starts_with_ci(admin, hint)) || (country && starts_with_ci(country, hint)) ||
                (cc && strcasecmp(cc, hint) == 0) ||
                (strlen(hint) == 2 && cc && strcasecmp(cc, "US") == 0)) {   // "MA", "TX", ...
                best = r;
                break;
            }
        }
    }
    bool ok = best != NULL;
    if (ok) {
        *lat = cJSON_GetNumberValue(cJSON_GetObjectItem(best, "latitude"));
        *lon = cJSON_GetNumberValue(cJSON_GetObjectItem(best, "longitude"));
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(best, "name"));
        const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(best, "admin1"));
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(best, "country"));
        snprintf(label, label_len, "%s%s%s%s%s", n ? n : location, a ? ", " : "", a ? a : "",
                 c ? ", " : "", c ? c : "");
    }
    cJSON_Delete(geo);
    return ok;
}

cJSON *weather_lookup(const char *location, int days, bool fahrenheit)
{
    if (!location || !location[0]) {
        return error("No location given and no home location is set. Ask the user where they "
                     "are, or tell them to set a home location on the setup page.");
    }
    if (days < 1) days = 1;
    if (days > 7) days = 7;

    double lat, lon;
    char label[128];
    if (!geocode(location, &lat, &lon, label, sizeof(label))) {
        return error("Couldn't find that place, or the weather service is unreachable.");
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=%d%s",
             lat, lon, days, fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
    cJSON *fc = get_json(url);
    if (!fc) return error("The weather service didn't answer. Try again in a moment.");

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "location", label);
    cJSON_AddStringToObject(out, "units", fahrenheit ? "temperature in Fahrenheit, wind in mph"
                                                     : "temperature in Celsius, wind in km/h");
    cJSON *cur = cJSON_GetObjectItem(fc, "current");
    if (cur) {
        cJSON *now = cJSON_AddObjectToObject(out, "now");
        cJSON_AddNumberToObject(now, "temperature", (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "temperature_2m")) + 0.5));
        cJSON_AddNumberToObject(now, "feels_like", (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "apparent_temperature")) + 0.5));
        cJSON_AddNumberToObject(now, "humidity_percent", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "relative_humidity_2m")));
        cJSON_AddNumberToObject(now, "wind", (int)(cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_speed_10m")) + 0.5));
        cJSON_AddStringToObject(now, "conditions", describe((int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "weather_code"))));
    }
    cJSON *daily = cJSON_GetObjectItem(fc, "daily");
    cJSON *dates = cJSON_GetObjectItem(daily, "time");
    cJSON *forecast = cJSON_AddArrayToObject(out, "forecast");
    for (int i = 0; i < cJSON_GetArraySize(dates); i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "date", cJSON_GetStringValue(cJSON_GetArrayItem(dates, i)));
        cJSON_AddNumberToObject(d, "high", (int)(cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), i)) + 0.5));
        cJSON_AddNumberToObject(d, "low", (int)(cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), i)) + 0.5));
        cJSON_AddStringToObject(d, "conditions", describe((int)cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "weather_code"), i))));
        cJSON *pp = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "precipitation_probability_max"), i);
        if (cJSON_IsNumber(pp)) cJSON_AddNumberToObject(d, "chance_of_precipitation_percent", (int)pp->valuedouble);
        cJSON_AddItemToArray(forecast, d);
    }
    cJSON_Delete(fc);
    ESP_LOGI(TAG, "weather for %s", label);
    return out;
}
