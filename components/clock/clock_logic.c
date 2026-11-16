// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L          // localtime_r, strcasecmp
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "clock_logic.h"

bool clk_parse_hhmm(const char *s, int *hour, int *minute)
{
    int h, m;
    char extra;
    if (!s || sscanf(s, "%d:%d%c", &h, &m, &extra) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *hour = h;
    *minute = m;
    return true;
}

bool clk_parse_repeat(const char *s, clk_repeat_t *out)
{
    if (!s || !*s || !strcasecmp(s, "once") || !strcasecmp(s, "none")) { *out = CLK_ONCE; return true; }
    if (!strcasecmp(s, "daily") || !strcasecmp(s, "every day") || !strcasecmp(s, "everyday")) { *out = CLK_DAILY; return true; }
    if (!strcasecmp(s, "weekdays")) { *out = CLK_WEEKDAYS; return true; }
    if (!strcasecmp(s, "weekends")) { *out = CLK_WEEKENDS; return true; }
    return false;
}

const char *clk_repeat_name(clk_repeat_t r)
{
    switch (r) {
    case CLK_DAILY: return "daily";
    case CLK_WEEKDAYS: return "weekdays";
    case CLK_WEEKENDS: return "weekends";
    default: return "once";
    }
}

bool clk_parse_date(const char *s, int *y, int *m, int *d)
{
    char extra;
    if (!s || sscanf(s, "%d-%d-%d%c", y, m, d, &extra) != 3) return false;
    return *y >= 2020 && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

static bool day_matches(const struct tm *t, clk_repeat_t r)
{
    bool weekend = t->tm_wday == 0 || t->tm_wday == 6;
    switch (r) {
    case CLK_WEEKDAYS: return !weekend;
    case CLK_WEEKENDS: return weekend;
    default: return true;
    }
}

/** hour:minute on the day `days_ahead` from `now`'s local date (DST-safe via mktime). */
static time_t at_day(time_t now, int days_ahead, int hour, int minute, struct tm *out)
{
    struct tm t;
    localtime_r(&now, &t);
    t.tm_mday += days_ahead;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t when = mktime(&t);                 // normalises month/year rollover too
    if (out) localtime_r(&when, out);
    return when;
}

time_t clk_next_alarm(time_t now, int hour, int minute, clk_repeat_t repeat, const char *date)
{
    if (repeat == CLK_ONCE && date && *date) {
        int y, m, d;
        if (!clk_parse_date(date, &y, &m, &d)) return 0;
        struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d,
                        .tm_hour = hour, .tm_min = minute, .tm_isdst = -1 };
        time_t when = mktime(&t);
        return when > now ? when : 0;
    }
    for (int day = 0; day <= 7; day++) {
        struct tm t;
        time_t when = at_day(now, day, hour, minute, &t);
        if (when > now && day_matches(&t, repeat)) return when;
    }
    return 0;
}

void clk_format_hms(int64_t s, char *buf, size_t len)
{
    if (s < 0) s = 0;
    int64_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h) snprintf(buf, len, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec);
    else snprintf(buf, len, "%lld:%02lld", (long long)m, (long long)sec);
}

void clk_sw_reset(clk_stopwatch_t *s) { memset(s, 0, sizeof(*s)); }

void clk_sw_start(clk_stopwatch_t *s, int64_t now)
{
    if (s->running) return;
    s->running = true;
    s->started_ms = now;
}

void clk_sw_stop(clk_stopwatch_t *s, int64_t now)
{
    if (!s->running) return;
    s->accumulated_ms += now - s->started_ms;
    s->running = false;
}

int64_t clk_sw_elapsed(const clk_stopwatch_t *s, int64_t now)
{
    return s->accumulated_ms + (s->running ? now - s->started_ms : 0);
}

bool clk_sw_lap(clk_stopwatch_t *s, int64_t now)
{
    if (s->laps >= CLK_MAX_LAPS) return false;
    s->lap_ms[s->laps++] = clk_sw_elapsed(s, now);
    return true;
}
