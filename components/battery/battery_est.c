// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "battery_est.h"

#define TAU_S       120.0f      // moving-average time constant
#define CHARGE_JUMP 3           // percent rise that counts as charging
#define STALE_US    (10LL * 60 * 1000000)

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

float batt_trimmed_mean(int *mv, int n, float *spread)
{
    if (n < 4) { *spread = 0; return n ? (float)mv[0] : 0; }
    qsort(mv, n, sizeof(int), cmp_int);
    int lo = n / 4, hi = n - n / 4;
    long sum = 0;
    for (int k = lo; k < hi; k++) sum += mv[k];
    *spread = (mv[hi - 1] - mv[lo]) / 2.0f;
    return (float)sum / (hi - lo);
}

float batt_divider_for(float pin_mv)
{
    if (pin_mv >= 1450 && pin_mv <= 2150) return 2.0f;   // 2:1 -> 2.9-4.3 V
    if (pin_mv >= 980 && pin_mv < 1450) return 3.0f;     // 3:1 -> 2.9-4.35 V
    return 0;
}

int batt_percent_for(float v)
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

void batt_lock_reset(batt_lock_t *l)
{
    l->lock_pin = -1;
    l->lock_div = 0;
    l->cand_pin = -1;
    l->cand_div = 0;
    l->agree = 0;
}

int batt_lock_choose(batt_lock_t *l, const float pin_mv[2], float *div, bool *locked_now)
{
    *locked_now = false;
    *div = 0;
    if (l->lock_pin >= 0) {
        if (batt_divider_for(pin_mv[l->lock_pin]) > 0) {
            *div = l->lock_div;
            return l->lock_pin;
        }
        return -1;                               // locked pin reads no battery
    }
    int pin = -1;
    for (int i = 0; i < 2 && pin < 0; i++) {     // prefer the schematic's pin (index 0)
        float d = batt_divider_for(pin_mv[i]);
        if (d > 0) { pin = i; *div = d; }
    }
    if (pin >= 0 && pin == l->cand_pin && *div == l->cand_div) {
        if (++l->agree >= BATT_LOCK_AFTER) {
            l->lock_pin = pin;
            l->lock_div = *div;
            *locked_now = true;
        }
    } else {
        l->cand_pin = pin;
        l->cand_div = *div;
        l->agree = pin >= 0 ? 1 : 0;
    }
    return pin;
}

void batt_est_reset(batt_est_t *e)
{
    memset(e, 0, sizeof(*e));
    e->shown_pct = -1;
}

static void history_add(batt_est_t *e, float v, int64_t now)
{
    if (v > e->max_v) e->max_v = v;
    if (!e->minute_start_us) e->minute_start_us = now;
    e->minute_sum += v;
    e->minute_n++;
    if (now - e->minute_start_us >= 60LL * 1000000) {
        e->hist[e->hist_pos] = e->minute_sum / e->minute_n;
        e->hist_pos = (e->hist_pos + 1) % BATT_HIST_MINUTES;
        if (e->hist_len < BATT_HIST_MINUTES) e->hist_len++;
        e->minute_sum = 0;
        e->minute_n = 0;
        e->minute_start_us = now;
    }
}

#define WARMUP_US (120LL * 1000000)

void batt_est_update(batt_est_t *e, float v, bool busy, int64_t now)
{
    if (!e->start_us) e->start_us = now;
    bool stale = e->avg_us && now - e->avg_us > STALE_US;
    if (!busy || e->avg_v == 0 || stale) {
        if (e->avg_v == 0 || (e->seeded_busy && !busy)) {
            // First value, or the first rest reading after a sagging (busy) first value.
            e->avg_v = v;
            e->seeded_busy = busy;
        } else {
            float dt = (now - e->avg_us) / 1e6f;
            float a = 1.0f - expf(-dt / TAU_S);
            e->avg_v += a * (v - e->avg_v);
        }
        e->avg_us = now;
        history_add(e, e->avg_v, now);
    }
    // Steady percentage: falls while discharging, rises only when clearly charging.
    // During the first two minutes it settles freely in either direction.
    int pct = batt_percent_for(e->avg_v);
    bool warming_up = now - e->start_us < WARMUP_US;
    if (e->shown_pct < 0 || warming_up || pct < e->shown_pct || pct >= e->shown_pct + CHARGE_JUMP) {
        e->shown_pct = pct;
    }
}

bool batt_est_slope(const batt_est_t *e, int minutes, float *out)
{
    if (e->hist_len < 6) return false;
    if (minutes > e->hist_len) minutes = e->hist_len;
    if (minutes < 2) return false;
    float newest = e->hist[(e->hist_pos - 1 + BATT_HIST_MINUTES) % BATT_HIST_MINUTES];
    float oldest = e->hist[(e->hist_pos - minutes + BATT_HIST_MINUTES) % BATT_HIST_MINUTES];
    *out = (newest - oldest) * 60.0f / (minutes - 1);
    return true;
}

bool batt_est_on_battery(const batt_est_t *e)
{
    if (e->avg_v == 0) return false;
    float slope;
    if (batt_est_slope(e, 15, &slope)) {
        if (slope > 0.01f) return false;                          // charging
        if (e->avg_v >= 4.15f && slope > -0.01f) return false;    // full, on USB power
        return true;
    }
    return e->avg_v < 4.10f;            // no history yet: below full charge = on battery
}
