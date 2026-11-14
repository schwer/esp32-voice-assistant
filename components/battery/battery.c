// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
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
