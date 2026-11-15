// SPDX-License-Identifier: MIT
//
// Battery estimation maths, separate from the ADC so it can be unit-tested on the host
// (tests/host). No ESP-IDF dependencies.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BATT_HIST_MINUTES 120

/** Trimmed mean of the middle half (sorts `mv` in place). *spread = half that range. */
float batt_trimmed_mean(int *mv, int n, float *spread);
/** Divider implied by a sense-pin voltage (2 or 3); 0 if it can't be one lithium cell. */
float batt_divider_for(float pin_mv);
/** Lithium-ion open-circuit voltage -> percent. */
int batt_percent_for(float volts);

/** Which sense pin / divider; locked once LOCK_AFTER readings in a row agree. */
#define BATT_LOCK_AFTER 6
typedef struct {
    int lock_pin;        // -1 until locked
    float lock_div;
    int cand_pin;
    float cand_div;
    int agree;
} batt_lock_t;

void batt_lock_reset(batt_lock_t *l);
/** Pick the pin for this reading (index, or -1 for none) and its divider. *locked_now on lock. */
int batt_lock_choose(batt_lock_t *l, const float pin_mv[2], float *div, bool *locked_now);

/** Steady, at-rest estimate with history. */
typedef struct {
    float avg_v;                  // time-averaged rest voltage (0 = none yet)
    int64_t avg_us;
    int shown_pct;                // -1 = none yet
    float hist[BATT_HIST_MINUTES];
    int hist_len, hist_pos;
    float minute_sum;
    int minute_n;
    int64_t minute_start_us;
    float max_v;
} batt_est_t;

void batt_est_reset(batt_est_t *e);
/**
 * Feed one reading (volts at the battery). Readings taken while busy (speaker or
 * conversation) are ignored unless the estimate would otherwise go stale (10 min).
 */
void batt_est_update(batt_est_t *e, float volts, bool busy, int64_t now_us);
/** Voltage change per hour over the last `minutes` of history (needs >= 6 minutes). */
bool batt_est_slope(const batt_est_t *e, int minutes, float *v_per_hour);
/** Running from the battery (discharging) rather than USB power. */
bool batt_est_on_battery(const batt_est_t *e);
