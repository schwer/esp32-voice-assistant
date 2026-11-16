// SPDX-License-Identifier: MIT
//
// Alarm, timer and stopwatch maths. No ESP-IDF dependencies (unit-tested in tests/host).
// Times are wall-clock time_t in the local time zone (TZ), so mktime() handles
// daylight-saving changes.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum { CLK_ONCE, CLK_DAILY, CLK_WEEKDAYS, CLK_WEEKENDS } clk_repeat_t;

/** "07:30", "7:30", "19:05" -> hour/minute (24-hour). */
bool clk_parse_hhmm(const char *s, int *hour, int *minute);
/** "once" / "daily" / "every day" / "weekdays" / "weekends" (case-insensitive). NULL/"" = once. */
bool clk_parse_repeat(const char *s, clk_repeat_t *out);
const char *clk_repeat_name(clk_repeat_t r);
/** "2026-11-20" -> year/month/day. */
bool clk_parse_date(const char *s, int *y, int *m, int *d);

/**
 * When an alarm at hour:minute fires next, strictly after `now`.
 * `date` (YYYY-MM-DD, only for CLK_ONCE) pins it to that day; returns 0 if that's past.
 */
time_t clk_next_alarm(time_t now, int hour, int minute, clk_repeat_t repeat, const char *date);

/** "1:05:03" style h:mm:ss (or m:ss under an hour). */
void clk_format_hms(int64_t seconds, char *buf, size_t len);

#define CLK_MAX_LAPS 10
typedef struct {
    bool running;
    int64_t started_ms;       // when the current run started
    int64_t accumulated_ms;   // time from earlier runs
    int laps;
    int64_t lap_ms[CLK_MAX_LAPS];
} clk_stopwatch_t;

void clk_sw_reset(clk_stopwatch_t *s);
void clk_sw_start(clk_stopwatch_t *s, int64_t now_ms);
void clk_sw_stop(clk_stopwatch_t *s, int64_t now_ms);
int64_t clk_sw_elapsed(const clk_stopwatch_t *s, int64_t now_ms);
/** Record a lap (elapsed time at this moment); false when the lap list is full. */
bool clk_sw_lap(clk_stopwatch_t *s, int64_t now_ms);
