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
#include "battery_est.h"

static const char *TAG = "battery";

#define SAMPLES 64

static const int PINS[2] = { 1, 8 };
static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_chan[2];
static adc_cali_handle_t s_cali;
static bool s_ok;
static SemaphoreHandle_t s_lock;

// Guarded by s_lock (the maths lives in battery_est.c, unit-tested on the host)
static battery_status_t s_st;
static batt_est_t s_est;
static batt_lock_t s_pin;

// ---------------------------------------------------------------- setup

static void load_lock(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READONLY, &h) != ESP_OK) return;
    int8_t pin = -1;
    uint8_t div = 0;
    if (nvs_get_i8(h, "pin", &pin) == ESP_OK && nvs_get_u8(h, "div", &div) == ESP_OK &&
        pin >= 0 && pin < 2 && (div == 2 || div == 3)) {
        s_pin.lock_pin = pin;
        s_pin.lock_div = div;
        ESP_LOGI(TAG, "using GPIO%d with a %d:1 divider (remembered)", PINS[pin], div);
    }
    nvs_close(h);
}

static void save_lock(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "pin", (int8_t)s_pin.lock_pin);
    nvs_set_u8(h, "div", (uint8_t)s_pin.lock_div);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    batt_est_reset(&s_est);
    batt_lock_reset(&s_pin);
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

/** 64 samples spread over a few ms; trimmed mean of the middle half. */
static float read_mv(int i, float *spread)
{
    int mv[SAMPLES], n = 0;
    for (int k = 0; k < SAMPLES; k++) {
        int raw, v;
        if (adc_oneshot_read(s_adc, s_chan[i], &raw) != ESP_OK) continue;
        if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &v) != ESP_OK) v = raw * 3100 / 4095;
        mv[n++] = v;
        if ((k & 15) == 15) vTaskDelay(1);
    }
    if (n < 8) { *spread = 0; return 0; }
    return batt_trimmed_mean(mv, n, spread);
}

void battery_tick(bool busy)
{
    if (!s_ok) return;
    float pin_mv[2], spread[2];
    for (int i = 0; i < 2; i++) pin_mv[i] = read_mv(i, &spread[i]);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    float div;
    bool locked_now;
    int pin = batt_lock_choose(&s_pin, pin_mv, &div, &locked_now);
    s_st.pin_mv[0] = pin_mv[0];
    s_st.pin_mv[1] = pin_mv[1];
    s_st.locked = s_pin.lock_pin >= 0;
    if (pin < 0) {
        s_st.present = false;
        batt_est_reset(&s_est);
        xSemaphoreGive(s_lock);
        return;
    }
    float v = pin_mv[pin] * div / 1000.0f;
    batt_est_update(&s_est, v, busy, esp_timer_get_time());
    s_st.present = true;
    s_st.gpio = PINS[pin];
    s_st.divider = div;
    s_st.last_raw_v = v;
    s_st.spread_mv = spread[pin] * div;
    s_st.volts = s_est.avg_v;
    s_st.percent = s_est.shown_pct;
    xSemaphoreGive(s_lock);

    if (locked_now) {
        save_lock();
        ESP_LOGI(TAG, "battery found on GPIO%d with a %.0f:1 divider; locked", PINS[pin], div);
    }
}

void battery_read(battery_status_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- reporting

bool battery_on_battery(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_st.present && batt_est_on_battery(&s_est);
    xSemaphoreGive(s_lock);
    return on;
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

    float slope, max_v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have_slope = batt_est_slope(&s_est, 30, &slope);
    max_v = s_est.max_v;
    xSemaphoreGive(s_lock);
    if (have_slope) {
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
    snprintf(v, sizeof(v), "%.2f", max_v);
    cJSON_AddStringToObject(h, "highest_voltage_seen", v);
    const char *assessment;
    if (max_v >= 4.12f) assessment = "Reaches a normal full-charge voltage, which is consistent with a healthy cell.";
    else if (max_v >= 4.0f) assessment = "Hasn't reached full-charge voltage since startup; charge it fully to judge its health.";
    else assessment = "Only partly charged since startup, so its health can't be judged yet.";
    cJSON_AddStringToObject(h, "assessment", assessment);
    cJSON_AddStringToObject(h, "limits", "This board has no fuel-gauge chip, so capacity and wear are estimates "
                                          "from voltage only. A battery that drains much faster than before is wearing out.");
    cJSON_AddNumberToObject(r, "uptime_minutes", (int)(esp_timer_get_time() / 60000000));
    return r;
}
