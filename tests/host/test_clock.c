// SPDX-License-Identifier: MIT
// Alarms, timers and stopwatch maths (components/clock/clock_logic.c).
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "clock_logic.h"
#include "test.h"

static time_t local(int y, int mo, int d, int h, int mi)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return mktime(&t);
}

static void expect_local(time_t when, int y, int mo, int d, int h, int mi)
{
    struct tm t;
    localtime_r(&when, &t);
    CHECK(t.tm_year + 1900 == y && t.tm_mon + 1 == mo && t.tm_mday == d && t.tm_hour == h && t.tm_min == mi);
    if (!(t.tm_year + 1900 == y && t.tm_mon + 1 == mo && t.tm_mday == d && t.tm_hour == h && t.tm_min == mi)) {
        printf("    got %04d-%02d-%02d %02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    }
}

static void parsing(void)
{
    int h, m;
    CHECK(clk_parse_hhmm("07:30", &h, &m) && h == 7 && m == 30);
    CHECK(clk_parse_hhmm("7:05", &h, &m) && h == 7 && m == 5);
    CHECK(clk_parse_hhmm("23:59", &h, &m));
    CHECK(!clk_parse_hhmm("24:00", &h, &m));
    CHECK(!clk_parse_hhmm("7:30pm", &h, &m));            // Gemini must convert to 24-hour
    CHECK(!clk_parse_hhmm("seven", &h, &m));
    CHECK(!clk_parse_hhmm(NULL, &h, &m));
    clk_repeat_t r;
    CHECK(clk_parse_repeat(NULL, &r) && r == CLK_ONCE);
    CHECK(clk_parse_repeat("Every Day", &r) && r == CLK_DAILY);
    CHECK(clk_parse_repeat("weekdays", &r) && r == CLK_WEEKDAYS);
    CHECK(!clk_parse_repeat("fortnightly", &r));
    int y, mo, d;
    CHECK(clk_parse_date("2026-11-20", &y, &mo, &d) && y == 2026 && mo == 11 && d == 20);
    CHECK(!clk_parse_date("20/11/2026", &y, &mo, &d));
}

static void once_alarms(void)
{
    time_t now = local(2026, 11, 16, 22, 0);                     // Monday 22:00
    expect_local(clk_next_alarm(now, 23, 30, CLK_ONCE, NULL), 2026, 11, 16, 23, 30);   // later today
    expect_local(clk_next_alarm(now, 7, 0, CLK_ONCE, NULL), 2026, 11, 17, 7, 0);       // tomorrow
    expect_local(clk_next_alarm(now, 22, 0, CLK_ONCE, NULL), 2026, 11, 17, 22, 0);     // "now" counts as past
    expect_local(clk_next_alarm(now, 9, 15, CLK_ONCE, "2026-11-20"), 2026, 11, 20, 9, 15);
    CHECK(clk_next_alarm(now, 9, 15, CLK_ONCE, "2026-11-01") == 0);                     // date in the past
    CHECK(clk_next_alarm(now, 9, 15, CLK_ONCE, "nonsense") == 0);
}

static void repeating_alarms(void)
{
    time_t fri_night = local(2026, 11, 20, 21, 0);               // Friday
    expect_local(clk_next_alarm(fri_night, 7, 0, CLK_WEEKDAYS, NULL), 2026, 11, 23, 7, 0);   // Monday
    expect_local(clk_next_alarm(fri_night, 9, 0, CLK_WEEKENDS, NULL), 2026, 11, 21, 9, 0);   // Saturday
    expect_local(clk_next_alarm(fri_night, 7, 0, CLK_DAILY, NULL), 2026, 11, 21, 7, 0);
    time_t sun = local(2026, 11, 22, 10, 0);
    expect_local(clk_next_alarm(sun, 9, 0, CLK_WEEKENDS, NULL), 2026, 11, 28, 9, 0);          // next Saturday
    time_t new_year = local(2026, 12, 31, 23, 0);
    expect_local(clk_next_alarm(new_year, 6, 30, CLK_DAILY, NULL), 2027, 1, 1, 6, 30);        // year rollover
}

static void daylight_saving_changes(void)
{
    // US Eastern: clocks go forward 2027-03-14 02:00 -> 03:00, back 2026-11-01 02:00 -> 01:00.
    time_t sat = local(2027, 3, 13, 22, 0);
    time_t next = clk_next_alarm(sat, 7, 0, CLK_DAILY, NULL);
    expect_local(next, 2027, 3, 14, 7, 0);                        // still 7:00 on the clock
    CHECK(next - sat == 8 * 3600);                                // only 8 real hours (one skipped)
    time_t back = local(2026, 10, 31, 22, 0);
    next = clk_next_alarm(back, 7, 0, CLK_DAILY, NULL);
    expect_local(next, 2026, 11, 1, 7, 0);
    CHECK(next - back == 10 * 3600);                              // 10 real hours (one repeated)
}

static void formatting(void)
{
    char b[32];
    clk_format_hms(5, b, sizeof(b));     CHECK(strcmp(b, "0:05") == 0);
    clk_format_hms(605, b, sizeof(b));   CHECK(strcmp(b, "10:05") == 0);
    clk_format_hms(3725, b, sizeof(b));  CHECK(strcmp(b, "1:02:05") == 0);
    clk_format_hms(-3, b, sizeof(b));    CHECK(strcmp(b, "0:00") == 0);
}

static void stopwatch(void)
{
    clk_stopwatch_t s;
    clk_sw_reset(&s);
    CHECK(clk_sw_elapsed(&s, 1000) == 0);
    clk_sw_start(&s, 1000);
    clk_sw_start(&s, 1500);                                       // already running: ignored
    CHECK(clk_sw_elapsed(&s, 4000) == 3000);
    CHECK(clk_sw_lap(&s, 4000) && s.lap_ms[0] == 3000);
    clk_sw_stop(&s, 5000);
    CHECK(clk_sw_elapsed(&s, 99000) == 4000);                     // paused
    clk_sw_start(&s, 100000);
    CHECK(clk_sw_elapsed(&s, 101000) == 5000);                    // resumes from 4 s
    for (int i = 1; i < CLK_MAX_LAPS; i++) CHECK(clk_sw_lap(&s, 101000));
    CHECK(!clk_sw_lap(&s, 101000));                               // lap list full
    clk_sw_reset(&s);
    CHECK(!s.running && s.laps == 0 && clk_sw_elapsed(&s, 200000) == 0);
}

int main(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);                    // a time zone with DST
    tzset();
    RUN(parsing);
    RUN(once_alarms);
    RUN(repeating_alarms);
    RUN(daylight_saving_changes);
    RUN(formatting);
    RUN(stopwatch);
    TEST_MAIN_END("clock");
}
