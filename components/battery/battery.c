// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "battery.h"

static const char *TAG = "battery";

static const int PINS[2] = { 1, 8 };
static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan[2];
static adc_cali_handle_t s_cali;
static bool s_ok;
static float s_smooth;                 // smoothed battery volts (0 = none yet)

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    for (int i = 0; i < 2; i++) {
        adc_unit_t u;
        if (adc_oneshot_io_to_channel(PINS[i], &u, &s_chan[i]) != ESP_OK) return ESP_FAIL;
        adc_oneshot_config_channel(s_adc, s_chan[i], &ch);
    }
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
    s_ok = true;
    return ESP_OK;
}

static float read_mv(int i)
{
    int sum = 0, n = 0;
    for (int k = 0; k < 8; k++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan[i], &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;
        n++;
    }
    return n ? (float)sum / n : 0;
}

/** Infer the divider from the pin voltage; 0 if the reading isn't a lithium cell. */
static float divider_for(float pin_mv)
{
    if (pin_mv >= 1450 && pin_mv <= 2150) return 2.0f;   // 2:1 -> 2.9-4.3 V
    if (pin_mv >= 980 && pin_mv < 1450) return 3.0f;     // 3:1 -> 2.9-4.35 V
    return 0;
}

static int percent_for(float v)
{
    static const float curve[][2] = {
        {4.20f, 100}, {4.10f, 90}, {4.00f, 78}, {3.90f, 65}, {3.80f, 50}, {3.75f, 40},
        {3.70f, 30}, {3.65f, 20}, {3.60f, 12}, {3.55f, 8}, {3.50f, 5}, {3.40f, 2}, {3.30f, 0},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (v >= curve[0][0]) return 100;
    for (int i = 1; i < n; i++) {
        if (v >= curve[i][0]) {
            float t = (v - curve[i][0]) / (curve[i - 1][0] - curve[i][0]);
            return (int)(curve[i][1] + t * (curve[i - 1][1] - curve[i][1]) + 0.5f);
        }
    }
    return 0;
}

void battery_read(battery_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_ok) return;
    for (int i = 0; i < 2; i++) out->pin_mv[i] = read_mv(i);

    // Prefer the schematic's pin; fall back to the other if only it looks like a battery.
    for (int i = 0; i < 2 && !out->present; i++) {
        float div = divider_for(out->pin_mv[i]);
        if (div > 0) {
            float v = out->pin_mv[i] * div / 1000.0f;
            // Smooth out the dips when the speaker draws current.
            s_smooth = s_smooth > 0 ? s_smooth * 0.8f + v * 0.2f : v;
            out->present = true;
            out->gpio = PINS[i];
            out->divider = div;
            out->volts = s_smooth;
            out->percent = percent_for(s_smooth);
        }
    }
    if (!out->present) s_smooth = 0;
    static bool logged;
    if (!logged) {
        logged = true;
        ESP_LOGI(TAG, "GPIO1 %.0f mV, GPIO8 %.0f mV -> %s", out->pin_mv[0], out->pin_mv[1],
                 out->present ? "battery found" : "no battery reading");
    }
}

// ---------------------------------------------------------------- history

#define HIST_MINUTES 120                 // one averaged reading per minute, last two hours
static float s_hist[HIST_MINUTES];
static int s_hist_len, s_hist_pos;
static float s_minute_sum;
static int s_minute_n;
static int64_t s_minute_start_us;
static float s_max_full_v;               // highest voltage seen (full-charge indicator)
static int64_t s_on_battery_since_us;    // when it last started discharging (0 = unknown)

void battery_tick(void)
{
    battery_status_t b;
    battery_read(&b);
    if (!b.present) return;
    if (b.volts > s_max_full_v) s_max_full_v = b.volts;

    int64_t now = esp_timer_get_time();
    if (!s_minute_start_us) s_minute_start_us = now;
    s_minute_sum += b.volts;
    s_minute_n++;
    if (now - s_minute_start_us >= 60LL * 1000000) {
        s_hist[s_hist_pos] = s_minute_sum / s_minute_n;
        s_hist_pos = (s_hist_pos + 1) % HIST_MINUTES;
        if (s_hist_len < HIST_MINUTES) s_hist_len++;
        s_minute_sum = 0;
        s_minute_n = 0;
        s_minute_start_us = now;
    }
}

/** Voltage change per hour over the last `minutes` (needs history). */
static bool slope_v_per_hour(int minutes, float *out)
{
    if (s_hist_len < 6) return false;
    if (minutes > s_hist_len) minutes = s_hist_len;
    float newest = s_hist[(s_hist_pos - 1 + HIST_MINUTES) % HIST_MINUTES];
    float oldest = s_hist[(s_hist_pos - minutes + HIST_MINUTES) % HIST_MINUTES];
    *out = (newest - oldest) * 60.0f / (minutes - 1);
    return true;
}

cJSON *battery_report(void)
{
    cJSON *r = cJSON_CreateObject();
    battery_status_t b;
    battery_read(&b);
    if (!b.present) {
        cJSON_AddBoolToObject(r, "battery_connected", false);
        cJSON_AddStringToObject(r, "note", "No battery reading: the board is probably running on USB "
                                           "power without a battery, or battery sensing isn't working.");
        return r;
    }
    char v[16];
    cJSON_AddBoolToObject(r, "battery_connected", true);
    cJSON_AddNumberToObject(r, "percent", b.percent);
    snprintf(v, sizeof(v), "%.2f", b.volts);
    cJSON_AddStringToObject(r, "voltage", v);

    float slope;
    int64_t now = esp_timer_get_time();
    if (slope_v_per_hour(30, &slope)) {
        const char *trend = slope > 0.02f ? "charging"
                          : b.volts >= 4.15f && slope > -0.01f ? "full or on USB power"
                          : slope < -0.005f ? "discharging" : "steady";
        cJSON_AddStringToObject(r, "trend", trend);
        if (slope < -0.005f) {
            if (!s_on_battery_since_us) s_on_battery_since_us = now - 30LL * 60 * 1000000;
            // Rough: the usable range is about 4.2 V down to 3.3 V.
            float hours = (b.volts - 3.3f) / -slope;
            if (hours > 0 && hours < 100) cJSON_AddNumberToObject(r, "estimated_hours_left", (int)(hours * 10 + 0.5f) / 10.0);
        } else {
            s_on_battery_since_us = 0;
        }
        snprintf(v, sizeof(v), "%+.3f", slope);
        cJSON_AddStringToObject(r, "voltage_change_per_hour", v);
    } else {
        cJSON_AddStringToObject(r, "trend", "not enough history yet (needs a few minutes after startup)");
    }

    // Health: without a fuel-gauge chip the device can't measure capacity directly.
    cJSON *h = cJSON_AddObjectToObject(r, "health");
    snprintf(v, sizeof(v), "%.2f", s_max_full_v);
    cJSON_AddStringToObject(h, "highest_voltage_seen", v);
    const char *assessment;
    if (s_max_full_v >= 4.12f) assessment = "Reaches a normal full-charge voltage, which is consistent with a healthy cell.";
    else if (s_max_full_v >= 4.0f) assessment = "Hasn't reached full-charge voltage since startup; charge it fully to judge its health.";
    else assessment = "Only partly charged since startup, so its health can't be judged yet.";
    cJSON_AddStringToObject(h, "assessment", assessment);
    cJSON_AddStringToObject(h, "limits", "This board has no fuel-gauge chip, so capacity and wear are estimates "
                                          "from voltage only. A battery that drains much faster than before is wearing out.");
    cJSON_AddNumberToObject(r, "uptime_minutes", (int)(now / 60000000));
    return r;
}
