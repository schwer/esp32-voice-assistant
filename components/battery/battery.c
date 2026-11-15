// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs.h"
#include "battery.h"

static const char *TAG = "battery";

#define SAMPLES        64
#define TAU_S          120.0f           // moving-average time constant
#define LOCK_AFTER     6                // agreeing readings before the pin/divider is locked
#define CHARGE_JUMP    3                // percent rise that counts as charging
#define HIST_MINUTES   120              // one averaged reading per minute, last two hours

static const int PINS[2] = { 1, 8 };
static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan[2];
static adc_cali_handle_t s_cali;
static bool s_ok;
static SemaphoreHandle_t s_lock;

// Estimate (guarded by s_lock)
static battery_status_t s_st;
static float s_avg_v;                   // time-averaged rest voltage (0 = none yet)
static int64_t s_avg_us;                // time of the last update to s_avg_v
static int s_shown_pct = -1;            // steady percentage
static int s_lock_pin = -1;             // index into PINS once locked
static float s_lock_div;
static int s_agree, s_cand_pin = -1;
static float s_cand_div;

// History (for trend, time left and health)
static float s_hist[HIST_MINUTES];
static int s_hist_len, s_hist_pos;
static float s_minute_sum;
static int s_minute_n;
static int64_t s_minute_start_us;
static float s_max_v;

// ---------------------------------------------------------------- setup

static void load_lock(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READONLY, &h) != ESP_OK) return;
    int8_t pin = -1;
    uint8_t div = 0;
    if (nvs_get_i8(h, "pin", &pin) == ESP_OK && nvs_get_u8(h, "div", &div) == ESP_OK &&
        pin >= 0 && pin < 2 && (div == 2 || div == 3)) {
        s_lock_pin = pin;
        s_lock_div = div;
        ESP_LOGI(TAG, "using GPIO%d with a %d:1 divider (remembered)", PINS[pin], div);
    }
    nvs_close(h);
}

static void save_lock(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "pin", (int8_t)s_lock_pin);
    nvs_set_u8(h, "div", (uint8_t)s_lock_div);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
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
    load_lock();
    s_ok = true;
    return ESP_OK;
}

// ---------------------------------------------------------------- measuring

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/** 64 samples, trimmed mean of the middle half. *spread = half the middle half's range. */
static float read_mv(int i, float *spread)
{
    int mv[SAMPLES], n = 0;
    for (int k = 0; k < SAMPLES; k++) {
        int raw, v;
        if (adc_oneshot_read(s_adc, s_chan[i], &raw) != ESP_OK) continue;
        if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &v) != ESP_OK) v = raw * 3100 / 4095;
        mv[n++] = v;
        if ((k & 15) == 15) vTaskDelay(1);              // spread samples over a few ms
    }
    if (n < 8) { *spread = 0; return 0; }
    qsort(mv, n, sizeof(int), cmp_int);
    int lo = n / 4, hi = n - n / 4;
    long sum = 0;
    for (int k = lo; k < hi; k++) sum += mv[k];
    *spread = (mv[hi - 1] - mv[lo]) / 2.0f;
    return (float)sum / (hi - lo);
}

/** Divider implied by a pin voltage; 0 if it can't be a lithium cell. */
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

static void history_add(float v, int64_t now)
{
    if (v > s_max_v) s_max_v = v;
    if (!s_minute_start_us) s_minute_start_us = now;
    s_minute_sum += v;
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

void battery_tick(bool busy)
{
    if (!s_ok) return;
    float pin_mv[2], spread[2];
    for (int i = 0; i < 2; i++) pin_mv[i] = read_mv(i, &spread[i]);

    // Which pin and divider? Locked ones win; otherwise prefer the schematic's pin.
    int pin = -1;
    float div = 0;
    if (s_lock_pin >= 0) {
        if (divider_for(pin_mv[s_lock_pin]) > 0) { pin = s_lock_pin; div = s_lock_div; }
    } else {
        for (int i = 0; i < 2 && pin < 0; i++) {
            float d = divider_for(pin_mv[i]);
            if (d > 0) { pin = i; div = d; }
        }
        if (pin >= 0 && pin == s_cand_pin && div == s_cand_div) {
            if (++s_agree >= LOCK_AFTER) {
                s_lock_pin = pin;
                s_lock_div = div;
                save_lock();
                ESP_LOGI(TAG, "battery found on GPIO%d with a %.0f:1 divider; locked", PINS[pin], div);
            }
        } else {
            s_cand_pin = pin;
            s_cand_div = div;
            s_agree = pin >= 0 ? 1 : 0;
        }
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.pin_mv[0] = pin_mv[0];
    s_st.pin_mv[1] = pin_mv[1];
    s_st.locked = s_lock_pin >= 0;
    if (pin < 0) {
        s_st.present = false;
        s_avg_v = 0;
        s_shown_pct = -1;
        xSemaphoreGive(s_lock);
        return;
    }
    float v = pin_mv[pin] * div / 1000.0f;
    s_st.present = true;
    s_st.gpio = PINS[pin];
    s_st.divider = div;
    s_st.last_raw_v = v;
    s_st.spread_mv = spread[pin] * div;

    // Readings under load sag: only rest readings feed the estimate. If the device has
    // been busy for a long time, fall back to loaded readings rather than going stale.
    bool stale = s_avg_us && now - s_avg_us > 10LL * 60 * 1000000;
    if (!busy || s_avg_v == 0 || stale) {
        if (s_avg_v == 0) {
            s_avg_v = v;
        } else {
            float dt = (now - s_avg_us) / 1e6f;
            float a = 1.0f - expf(-dt / TAU_S);
            s_avg_v += a * (v - s_avg_v);
        }
        s_avg_us = now;
        history_add(s_avg_v, now);
    }

    // Steady percentage: falls while discharging, rises only when clearly charging.
    int pct = percent_for(s_avg_v);
    if (s_shown_pct < 0 || pct < s_shown_pct || pct >= s_shown_pct + CHARGE_JUMP) s_shown_pct = pct;
    s_st.volts = s_avg_v;
    s_st.percent = s_shown_pct;
    xSemaphoreGive(s_lock);
}

void battery_read(battery_status_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- reporting

/** Voltage change per hour over the last `minutes` of history. */
static bool slope_v_per_hour(int minutes, float *out)
{
    if (s_hist_len < 6) return false;
    if (minutes > s_hist_len) minutes = s_hist_len;
    float newest = s_hist[(s_hist_pos - 1 + HIST_MINUTES) % HIST_MINUTES];
    float oldest = s_hist[(s_hist_pos - minutes + HIST_MINUTES) % HIST_MINUTES];
    *out = (newest - oldest) * 60.0f / (minutes - 1);
    return true;
}

bool battery_on_battery(void)
{
    battery_status_t b;
    battery_read(&b);
    if (!b.present) return false;
    float slope;
    if (slope_v_per_hour(15, &slope)) {
        if (slope > 0.01f) return false;                         // charging
        if (b.volts >= 4.15f && slope > -0.01f) return false;    // full, on USB power
        return true;
    }
    return b.volts < 4.10f;            // no history yet: below full charge = on battery
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
    if (slope_v_per_hour(30, &slope)) {
        const char *trend = slope > 0.02f ? "charging"
                          : b.volts >= 4.15f && slope > -0.01f ? "full or on USB power"
                          : slope < -0.005f ? "discharging" : "steady";
        cJSON_AddStringToObject(r, "trend", trend);
        if (slope < -0.005f) {
            float hours = (b.volts - 3.3f) / -slope;            // usable range ~4.2 to 3.3 V
            if (hours > 0 && hours < 100) cJSON_AddNumberToObject(r, "estimated_hours_left", (int)(hours * 10 + 0.5f) / 10.0);
        }
        snprintf(v, sizeof(v), "%+.3f", slope);
        cJSON_AddStringToObject(r, "voltage_change_per_hour", v);
    } else {
        cJSON_AddStringToObject(r, "trend", "measuring (needs a few minutes after startup)");
    }

    cJSON *h = cJSON_AddObjectToObject(r, "health");
    snprintf(v, sizeof(v), "%.2f", s_max_v);
    cJSON_AddStringToObject(h, "highest_voltage_seen", v);
    const char *assessment;
    if (s_max_v >= 4.12f) assessment = "Reaches a normal full-charge voltage, which is consistent with a healthy cell.";
    else if (s_max_v >= 4.0f) assessment = "Hasn't reached full-charge voltage since startup; charge it fully to judge its health.";
    else assessment = "Only partly charged since startup, so its health can't be judged yet.";
    cJSON_AddStringToObject(h, "assessment", assessment);
    cJSON_AddStringToObject(h, "limits", "This board has no fuel-gauge chip, so capacity and wear are estimates "
                                          "from voltage only. A battery that drains much faster than before is wearing out.");
    cJSON_AddNumberToObject(r, "uptime_minutes", (int)(esp_timer_get_time() / 60000000));
    return r;
}
