// SPDX-License-Identifier: MIT
//
// Battery level from the board's battery-sense pin.
// Sources disagree on the pin (schematic: GPIO1 "BAT_ADC"; community pin map: GPIO8)
// and the divider ratio isn't documented, so both pins are read and the divider is
// inferred from the voltage range a single lithium cell (3.0-4.25 V) can produce.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool present;          // a plausible lithium cell reading was found
    float volts;           // battery voltage (smoothed)
    int percent;           // 0-100 estimate
    int gpio;              // pin the reading came from
    float divider;         // inferred divider ratio (2 or 3)
    float pin_mv[2];       // raw pin voltages: [0] = GPIO1, [1] = GPIO8 (diagnostics)
} battery_status_t;

esp_err_t battery_init(void);
/** Take a reading (cheap; call every few seconds). */
void battery_read(battery_status_t *out);
