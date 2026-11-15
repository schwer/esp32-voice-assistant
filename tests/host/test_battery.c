// SPDX-License-Identifier: MIT
// Battery estimation: noise rejection, divider/pin choice, steady percentage, trend.
#include <stdlib.h>
#include "battery_est.h"
#include "test.h"

#define S(x) ((int64_t)((x) * 1000000.0))   // seconds -> microseconds

static void trimmed_mean_ignores_spikes(void)
{
    int mv[64];
    for (int i = 0; i < 64; i++) mv[i] = 1900 + (i % 5) - 2;    // ~1900 mV +/- 2
    mv[3] = 3000; mv[10] = 200; mv[20] = 2600; mv[40] = 0;      // spikes
    float spread;
    float m = batt_trimmed_mean(mv, 64, &spread);
    CHECK_NEAR(m, 1900, 2);
    CHECK(spread < 5);
}

static void divider_detection_ranges(void)
{
    CHECK(batt_divider_for(1900) == 2.0f);     // 3.8 V on a 2:1 divider
    CHECK(batt_divider_for(1270) == 3.0f);     // 3.8 V on a 3:1 divider
    CHECK(batt_divider_for(5) == 0);           // nothing connected
    CHECK(batt_divider_for(3000) == 0);        // not a lithium cell
}

static void percent_curve_endpoints_and_monotonic(void)
{
    CHECK(batt_percent_for(4.25f) == 100);
    CHECK(batt_percent_for(4.20f) == 100);
    CHECK(batt_percent_for(3.30f) == 0);
    CHECK(batt_percent_for(3.00f) == 0);
    CHECK(batt_percent_for(3.80f) == 50);
    int prev = 101;
    for (float v = 4.25f; v >= 3.2f; v -= 0.005f) {
        int p = batt_percent_for(v);
        CHECK(p <= prev);
        prev = p;
    }
}

static void pin_locks_only_after_agreeing_readings(void)
{
    batt_lock_t l;
    batt_lock_reset(&l);
    float div;
    bool locked;
    const float pins[2] = { 1900, 10 };
    for (int i = 0; i < BATT_LOCK_AFTER - 1; i++) {
        CHECK(batt_lock_choose(&l, pins, &div, &locked) == 0);
        CHECK(!locked && l.lock_pin < 0);
    }
    CHECK(batt_lock_choose(&l, pins, &div, &locked) == 0);
    CHECK(locked && l.lock_pin == 0 && l.lock_div == 2.0f);
    // Once locked, a reading in the other divider's range doesn't change the divider.
    const float odd[2] = { 1300, 10 };
    CHECK(batt_lock_choose(&l, odd, &div, &locked) == 0);
    CHECK(div == 2.0f && !locked);
}

static void pin_lock_resets_when_readings_disagree(void)
{
    batt_lock_t l;
    batt_lock_reset(&l);
    float div;
    bool locked;
    const float a[2] = { 1900, 10 }, b[2] = { 1300, 10 };    // 2:1 vs 3:1 alternating
    for (int i = 0; i < 20; i++) batt_lock_choose(&l, i % 2 ? b : a, &div, &locked);
    CHECK(l.lock_pin < 0);
    const float other[2] = { 5, 1900 };                       // battery on GPIO8 only
    int pin = -1;
    for (int i = 0; i < BATT_LOCK_AFTER; i++) pin = batt_lock_choose(&l, other, &div, &locked);
    CHECK(pin == 1 && l.lock_pin == 1);
}

static void busy_readings_do_not_drag_the_level_down(void)
{
    batt_est_t e;
    batt_est_reset(&e);
    int64_t t = 0;
    for (int i = 0; i < 200; i++, t += S(5)) {
        bool busy = i % 3 == 0;
        batt_est_update(&e, busy ? 3.75f : 3.90f, busy, t);   // 150 mV sag while busy
    }
    CHECK_NEAR(e.avg_v, 3.90, 0.005);
    CHECK(e.shown_pct == batt_percent_for(3.90f));
}

static void busy_first_reading_is_replaced_by_the_first_rest_reading(void)
{
    batt_est_t e;
    batt_est_reset(&e);
    batt_est_update(&e, 3.70f, true, S(0));       // boot: speaking "Ready", voltage sags
    batt_est_update(&e, 3.90f, false, S(5));      // first rest reading
    CHECK_NEAR(e.avg_v, 3.90, 0.001);
    CHECK(e.shown_pct == batt_percent_for(3.90f));
}

static void noise_is_smoothed_and_level_never_rises_while_discharging(void)
{
    srand(1);
    batt_est_t e;
    batt_est_reset(&e);
    int64_t t = 0;
    int prev = 101, ups = 0, max_step = 0;
    for (int i = 0; i < 720; i++, t += S(5)) {                // one hour
        float truth = 3.92f - 0.06f * i / 720.0f;
        float noise = ((rand() % 2001) - 1000) / 1000.0f * 0.04f;   // +/- 40 mV
        batt_est_update(&e, truth + noise, false, t);
        if (i > 24) {
            if (e.shown_pct > prev) ups++;
            if (prev <= 100 && abs(prev - e.shown_pct) > max_step) max_step = abs(prev - e.shown_pct);
        }
        prev = e.shown_pct;
    }
    CHECK(ups == 0);
    CHECK(max_step <= 1);
    CHECK(abs(e.shown_pct - batt_percent_for(3.86f)) <= 3);
}

static void charging_raises_the_level(void)
{
    batt_est_t e;
    batt_est_reset(&e);
    int64_t t = 0;
    for (int i = 0; i < 60; i++, t += S(5)) batt_est_update(&e, 3.70f, false, t);
    int low = e.shown_pct;
    for (int i = 0; i < 400; i++, t += S(5)) batt_est_update(&e, 4.05f, false, t);  // plugged in
    CHECK(e.shown_pct >= low + 3);
}

static void trend_and_on_battery(void)
{
    batt_est_t e;
    batt_est_reset(&e);
    int64_t t = 0;
    float slope;
    CHECK(!batt_est_slope(&e, 30, &slope));                   // no history yet
    for (int i = 0; i < 12 * 30; i++, t += S(5)) batt_est_update(&e, 3.95f - 0.0002f * i, false, t);
    CHECK(batt_est_slope(&e, 30, &slope));
    CHECK(slope < -0.005f);                                   // discharging
    CHECK(batt_est_on_battery(&e));

    batt_est_reset(&e);
    t = 0;
    for (int i = 0; i < 12 * 30; i++, t += S(5)) batt_est_update(&e, 4.19f, false, t);
    CHECK(!batt_est_on_battery(&e));                          // full on USB power
}

int main(void)
{
    RUN(trimmed_mean_ignores_spikes);
    RUN(divider_detection_ranges);
    RUN(percent_curve_endpoints_and_monotonic);
    RUN(pin_locks_only_after_agreeing_readings);
    RUN(pin_lock_resets_when_readings_disagree);
    RUN(busy_readings_do_not_drag_the_level_down);
    RUN(busy_first_reading_is_replaced_by_the_first_rest_reading);
    RUN(noise_is_smoothed_and_level_never_rises_while_discharging);
    RUN(charging_raises_the_level);
    RUN(trend_and_on_battery);
    TEST_MAIN_END("battery");
}
