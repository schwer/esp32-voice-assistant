// SPDX-License-Identifier: MIT
//
// Battery level from the board's battery-sense pin.
//
// A single ADC reading is a poor guide to charge: the ESP32-S3 ADC is noisy (worse with
// Wi-Fi active), and a lithium cell's voltage sags while the speaker or radio draws
// current. So the estimate is built from:
//   - 64 samples per reading, trimmed mean (drops the noisiest quarter at each end)
//   - only "at rest" readings (not while talking or playing audio)
//   - a time-based moving average (2-minute time constant)
//   - a percentage that only falls while discharging (rises only when clearly charging)
//
// Sources disagree on the sense pin (schematic: GPIO1 "BAT_ADC"; community pin map:
// GPIO8) and the divider isn't documented, so both pins are checked and the pin and
// divider are locked (and remembered) once readings agree.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    bool present;          // a plausible lithium cell reading was found
    float volts;           // battery voltage at rest (averaged)
    int percent;           // 0-100 estimate (steady)
    int gpio;              // sense pin in use
    float divider;         // divider ratio in use (2 or 3)
    bool locked;           // pin and divider confirmed
    float pin_mv[2];       // latest raw pin voltages: [0] = GPIO1, [1] = GPIO8
    float spread_mv;       // noise of the latest raw reading (battery side, +/- mV)
    float last_raw_v;      // latest single reading (battery side), for comparison
} battery_status_t;

esp_err_t battery_init(void);
/**
 * Take a reading and update the estimate. Call every few seconds from one place.
 * busy = the speaker is playing or a conversation is running (readings sag under load).
 */
void battery_tick(bool busy);
/** Current estimate (no new measurement; cheap, safe to call from anywhere). */
void battery_read(battery_status_t *out);
/** Level, voltage, charging trend, estimated time left and health, for the assistant. */
cJSON *battery_report(void);
/** True when running from the battery (discharging), false on USB power or with no battery. */
bool battery_on_battery(void);
