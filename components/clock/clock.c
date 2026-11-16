// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.h"
#include "prompts.h"
#include "storage.h"
#include "clock.h"
#include "clock_logic.h"

static const char *TAG = "clock";

#define MAX_TIMERS   8
#define MAX_ALARMS   10
#define LABEL_MAX    40
#define ALARMS_PATH  STORAGE_ROOT "/alarms.json"
#define TIMER_RING_S 60
#define ALARM_RING_S 120

typedef struct {
    int id;
    char label[LABEL_MAX];
    int64_t end_us;              // esp_timer time it goes off
    int total_s;
} timer_t_;

typedef struct {
    int id;
    char label[LABEL_MAX];
    int hour, minute;
    clk_repeat_t repeat;
    char date[11];               // once on a specific day, or ""
    time_t next;                 // 0 = not scheduled yet (clock not set)
} alarm_t_;

static SemaphoreHandle_t s_lock;
static timer_t_ s_timers[MAX_TIMERS];
static int s_n_timers;
static alarm_t_ s_alarms[MAX_ALARMS];
static int s_n_alarms;
static int s_next_id = 1;
static clk_stopwatch_t s_sw;
static clock_ring_cb_t s_on_ring;

static volatile bool s_ringing, s_stop;
static clock_ring_t s_ring_kind;
static char s_ring_label[LABEL_MAX];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool clock_valid(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year > 120;     // set from the internet (SNTP)
}

// ---------------------------------------------------------------- persistence

static void save_alarms(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_n_alarms; i++) {
        alarm_t_ *a = &s_alarms[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", a->id);
        cJSON_AddStringToObject(o, "label", a->label);
        cJSON_AddNumberToObject(o, "hour", a->hour);
        cJSON_AddNumberToObject(o, "minute", a->minute);
        cJSON_AddStringToObject(o, "repeat", clk_repeat_name(a->repeat));
        cJSON_AddStringToObject(o, "date", a->date);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return;
    FILE *f = fopen(ALARMS_PATH ".tmp", "w");
    if (f) {
        bool ok = fputs(json, f) >= 0;
        ok &= fclose(f) == 0;
        if (ok) {
            remove(ALARMS_PATH);
            rename(ALARMS_PATH ".tmp", ALARMS_PATH);
        }
    }
    free(json);
}

static void load_alarms(void)
{
    FILE *f = fopen(ALARMS_PATH, "r");
    if (!f) return;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *arr = cJSON_Parse(buf);
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (s_n_alarms >= MAX_ALARMS) break;
        alarm_t_ *a = &s_alarms[s_n_alarms];
        memset(a, 0, sizeof(*a));
        a->id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "id"));
        strlcpy(a->label, cJSON_GetStringValue(cJSON_GetObjectItem(o, "label")) ?: "", sizeof(a->label));
        a->hour = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "hour"));
        a->minute = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "minute"));
        clk_parse_repeat(cJSON_GetStringValue(cJSON_GetObjectItem(o, "repeat")), &a->repeat);
        strlcpy(a->date, cJSON_GetStringValue(cJSON_GetObjectItem(o, "date")) ?: "", sizeof(a->date));
        if (a->id >= s_next_id) s_next_id = a->id + 1;
        s_n_alarms++;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "%d alarm(s) loaded", s_n_alarms);
}

// ---------------------------------------------------------------- ringing

static int16_t s_beep[16000 * 15 / 100];          // 150 ms, 880 Hz, faded edges

static void make_beep(void)
{
    size_t n = sizeof(s_beep) / sizeof(s_beep[0]), fade = 16000 / 200;
    for (size_t i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < fade) env = (float)i / fade;
        if (n - i < fade) env = (float)(n - i) / fade;
        s_beep[i] = (int16_t)(0.45f * 32767 * env * sinf(2 * 3.14159265f * 880 * i / 16000.0f));
    }
}

static void ring_task(void *arg)
{
    int limit_s = s_ring_kind == CLOCK_RING_ALARM ? ALARM_RING_S : TIMER_RING_S;
    int64_t start = now_ms();
    audio_flush_playback();                                   // cut off anything mid-sentence
    prompts_say_always(s_ring_kind == CLOCK_RING_ALARM ? PROMPT_ALARM_RINGING : PROMPT_TIMER_DONE);
    audio_wait_idle(5000);
    while (!s_stop && now_ms() - start < limit_s * 1000LL) {
        for (int b = 0; b < 3 && !s_stop; b++) {              // beep beep beep
            audio_play_16k(s_beep, sizeof(s_beep) / sizeof(s_beep[0]));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        for (int i = 0; i < 12 && !s_stop; i++) vTaskDelay(pdMS_TO_TICKS(100));   // pause: room to say "Jarvis"
    }
    audio_flush_playback();
    ESP_LOGI(TAG, "%s stopped (%s)", s_ring_kind == CLOCK_RING_ALARM ? "alarm" : "timer",
             s_stop ? "by the user" : "timed out");
    s_ringing = false;
    if (s_on_ring) s_on_ring(false, s_ring_kind, s_ring_label);
    vTaskDelete(NULL);
}

static void start_ringing(clock_ring_t kind, const char *label)
{
    if (s_ringing) return;                                    // one at a time; the next waits
    s_ring_kind = kind;
    strlcpy(s_ring_label, label, sizeof(s_ring_label));
    s_stop = false;
    s_ringing = true;
    ESP_LOGI(TAG, "%s going off: %s", kind == CLOCK_RING_ALARM ? "alarm" : "timer", label[0] ? label : "(no name)");
    if (s_on_ring) s_on_ring(true, kind, label);
    xTaskCreate(ring_task, "ring", 3072, NULL, 4, NULL);
}

bool clock_ringing(void) { return s_ringing; }

void clock_stop_ringing(void)
{
    if (!s_ringing) return;
    s_stop = true;
    audio_flush_playback();
}

// ---------------------------------------------------------------- scheduler

static void schedule_alarm(alarm_t_ *a, time_t now)
{
    a->next = clk_next_alarm(now, a->hour, a->minute, a->repeat, a->date[0] ? a->date : NULL);
}

static void clock_task(void *arg)
{
    bool was_valid = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t t_us = esp_timer_get_time();
        bool valid = clock_valid();
        time_t now = time(NULL);
        xSemaphoreTake(s_lock, portMAX_DELAY);

        // Timers
        for (int i = 0; i < s_n_timers && !s_ringing; i++) {
            if (t_us >= s_timers[i].end_us) {
                char label[LABEL_MAX];
                strlcpy(label, s_timers[i].label, sizeof(label));
                s_timers[i] = s_timers[--s_n_timers];
                start_ringing(CLOCK_RING_TIMER, label);
                break;
            }
        }
        // Alarms (need the real time)
        if (valid && !was_valid) {
            for (int i = 0; i < s_n_alarms; i++) schedule_alarm(&s_alarms[i], now);
        }
        was_valid = valid;
        bool changed = false;
        for (int i = 0; valid && i < s_n_alarms && !s_ringing; i++) {
            alarm_t_ *a = &s_alarms[i];
            if (a->next && now >= a->next) {
                bool missed = now - a->next > 10 * 60;           // e.g. the board was off
                char label[LABEL_MAX];
                strlcpy(label, a->label, sizeof(label));
                if (a->repeat == CLK_ONCE) {
                    s_alarms[i--] = s_alarms[--s_n_alarms];
                } else {
                    schedule_alarm(a, now);
                }
                changed = true;
                if (!missed) {
                    start_ringing(CLOCK_RING_ALARM, label);
                    break;
                }
            } else if (!a->next && a->repeat == CLK_ONCE && a->date[0]) {
                s_alarms[i--] = s_alarms[--s_n_alarms];          // dated alarm already past
                changed = true;
            }
        }
        if (changed) save_alarms();
        xSemaphoreGive(s_lock);
    }
}

esp_err_t clock_init(clock_ring_cb_t on_ring)
{
    s_on_ring = on_ring;
    s_lock = xSemaphoreCreateMutex();
    clk_sw_reset(&s_sw);
    make_beep();
    load_alarms();
    xTaskCreate(clock_task, "clock", 4096, NULL, 4, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------- tools

static cJSON *err(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return r;
}

static const char *arg_str(const cJSON *args, const char *k)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(args, k));
    return s ? s : "";
}

static double arg_num(const cJSON *args, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItem(args, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return atof(v->valuestring);
    return dflt;
}

static void fmt_local(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%A %H:%M", &tm);
}

static cJSON *timer_json(const timer_t_ *t, int64_t t_us)
{
    cJSON *o = cJSON_CreateObject();
    char hms[16];
    int64_t left = (t->end_us - t_us + 999999) / 1000000;
    clk_format_hms(left, hms, sizeof(hms));
    cJSON_AddNumberToObject(o, "id", t->id);
    cJSON_AddStringToObject(o, "label", t->label);
    cJSON_AddNumberToObject(o, "seconds_left", (double)left);
    cJSON_AddStringToObject(o, "time_left", hms);
    return o;
}

static cJSON *alarm_json(const alarm_t_ *a)
{
    cJSON *o = cJSON_CreateObject();
    char hm[8], when[32] = "when the clock is set";
    snprintf(hm, sizeof(hm), "%02d:%02d", a->hour, a->minute);
    if (a->next) fmt_local(a->next, when, sizeof(when));
    cJSON_AddNumberToObject(o, "id", a->id);
    cJSON_AddStringToObject(o, "label", a->label);
    cJSON_AddStringToObject(o, "time", hm);
    cJSON_AddStringToObject(o, "repeat", clk_repeat_name(a->repeat));
    if (a->date[0]) cJSON_AddStringToObject(o, "date", a->date);
    cJSON_AddStringToObject(o, "next", when);
    return o;
}

/** Index of the entry matching `id` or (case-insensitive) `label`, or -1. */
static int find(const char *label, int id, bool alarms)
{
    int n = alarms ? s_n_alarms : s_n_timers;
    for (int i = 0; i < n; i++) {
        int eid = alarms ? s_alarms[i].id : s_timers[i].id;
        const char *el = alarms ? s_alarms[i].label : s_timers[i].label;
        if ((id && eid == id) || (label[0] && strcasecmp(el, label) == 0)) return i;
    }
    return -1;
}

static cJSON *tool_locked(const char *name, const cJSON *args)
{
    int64_t t_us = esp_timer_get_time();
    if (!strcmp(name, "set_timer")) {
        double secs = arg_num(args, "seconds", 0);
        if (secs < 1 || secs > 24 * 3600) return err("Timers can be 1 second to 24 hours.");
        if (s_n_timers >= MAX_TIMERS) return err("Too many timers running (8). Cancel one first.");
        timer_t_ *t = &s_timers[s_n_timers++];
        t->id = s_next_id++;
        strlcpy(t->label, arg_str(args, "label"), sizeof(t->label));
        t->total_s = (int)secs;
        t->end_us = t_us + (int64_t)(secs * 1e6);
        cJSON *r = timer_json(t, t_us);
        cJSON_AddBoolToObject(r, "set", true);
        return r;
    }
    if (!strcmp(name, "list_timers")) {
        cJSON *r = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(r, "timers");
        for (int i = 0; i < s_n_timers; i++) cJSON_AddItemToArray(arr, timer_json(&s_timers[i], t_us));
        return r;
    }
    if (!strcmp(name, "cancel_timer")) {
        const char *label = arg_str(args, "label");
        if (!strcasecmp(label, "all")) {
            int n = s_n_timers;
            s_n_timers = 0;
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "cancelled", n);
            return r;
        }
        int i = find(label, (int)arg_num(args, "id", 0), false);
        if (i < 0 && s_n_timers == 1 && !label[0]) i = 0;        // "cancel the timer"
        if (i < 0) return err(s_n_timers ? "No timer by that name; ask which one." : "No timers are running.");
        cJSON *r = timer_json(&s_timers[i], t_us);
        cJSON_AddBoolToObject(r, "cancelled", true);
        s_timers[i] = s_timers[--s_n_timers];
        return r;
    }
    if (!strcmp(name, "set_alarm")) {
        int h, m;
        clk_repeat_t rep;
        if (!clk_parse_hhmm(arg_str(args, "time"), &h, &m)) return err("Time must be 24-hour HH:MM, e.g. 07:30.");
        if (!clk_parse_repeat(arg_str(args, "repeat"), &rep)) return err("Repeat must be once, daily, weekdays or weekends.");
        if (!clock_valid()) return err("My clock isn't set yet (it needs Wi-Fi once after starting). Try again in a minute.");
        if (s_n_alarms >= MAX_ALARMS) return err("Too many alarms (10). Cancel one first.");
        alarm_t_ a = { .id = s_next_id, .hour = h, .minute = m, .repeat = rep };
        strlcpy(a.label, arg_str(args, "label"), sizeof(a.label));
        const char *date = arg_str(args, "date");
        if (rep == CLK_ONCE && date[0]) strlcpy(a.date, date, sizeof(a.date));
        schedule_alarm(&a, time(NULL));
        if (!a.next) return err("That date and time has already passed.");
        s_next_id++;
        s_alarms[s_n_alarms++] = a;
        save_alarms();
        cJSON *r = alarm_json(&a);
        cJSON_AddBoolToObject(r, "set", true);
        return r;
    }
    if (!strcmp(name, "list_alarms")) {
        cJSON *r = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(r, "alarms");
        for (int i = 0; i < s_n_alarms; i++) cJSON_AddItemToArray(arr, alarm_json(&s_alarms[i]));
        return r;
    }
    if (!strcmp(name, "cancel_alarm")) {
        const char *label = arg_str(args, "label");
        if (!strcasecmp(label, "all")) {
            int n = s_n_alarms;
            s_n_alarms = 0;
            save_alarms();
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "cancelled", n);
            return r;
        }
        int i = find(label, (int)arg_num(args, "id", 0), true);
        if (i < 0 && s_n_alarms == 1 && !label[0]) i = 0;
        if (i < 0) return err(s_n_alarms ? "No alarm matches; list them and ask which one." : "No alarms are set.");
        cJSON *r = alarm_json(&s_alarms[i]);
        cJSON_AddBoolToObject(r, "cancelled", true);
        s_alarms[i] = s_alarms[--s_n_alarms];
        save_alarms();
        return r;
    }
    if (!strcmp(name, "stopwatch")) {
        const char *action = arg_str(args, "action");
        int64_t ms = now_ms();
        if (!strcasecmp(action, "start")) clk_sw_start(&s_sw, ms);
        else if (!strcasecmp(action, "stop") || !strcasecmp(action, "pause")) clk_sw_stop(&s_sw, ms);
        else if (!strcasecmp(action, "reset")) clk_sw_reset(&s_sw);
        else if (!strcasecmp(action, "lap")) {
            if (!s_sw.running) return err("The stopwatch isn't running.");
            if (!clk_sw_lap(&s_sw, ms)) return err("Lap list is full (10).");
        } else if (strcasecmp(action, "status") && action[0]) {
            return err("Action must be start, stop, lap, reset or status.");
        }
        cJSON *r = cJSON_CreateObject();
        char hms[16];
        int64_t el = clk_sw_elapsed(&s_sw, ms);
        clk_format_hms(el / 1000, hms, sizeof(hms));
        cJSON_AddBoolToObject(r, "running", s_sw.running);
        cJSON_AddStringToObject(r, "elapsed", hms);
        cJSON_AddNumberToObject(r, "elapsed_seconds", el / 1000.0);
        cJSON *laps = cJSON_AddArrayToObject(r, "laps");
        for (int i = 0; i < s_sw.laps; i++) {
            clk_format_hms(s_sw.lap_ms[i] / 1000, hms, sizeof(hms));
            cJSON_AddItemToArray(laps, cJSON_CreateString(hms));
        }
        return r;
    }
    return NULL;
}

cJSON *clock_tool(const char *name, const cJSON *args)
{
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *r = tool_locked(name, args);
    xSemaphoreGive(s_lock);
    return r;
}

cJSON *clock_snapshot(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!s_lock) return r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t_us = esp_timer_get_time();
    char line[96];
    if (s_ringing) {
        snprintf(line, sizeof(line), "%s%s%s", s_ring_kind == CLOCK_RING_ALARM ? "alarm" : "timer",
                 s_ring_label[0] ? ": " : "", s_ring_label);
        cJSON_AddStringToObject(r, "ringing", line);
    }
    for (int i = 0; i < s_n_timers; i++) {
        char key[24], hms[16];
        clk_format_hms((s_timers[i].end_us - t_us + 999999) / 1000000, hms, sizeof(hms));
        snprintf(key, sizeof(key), "timer %d", i + 1);
        snprintf(line, sizeof(line), "%s%s%s left", s_timers[i].label, s_timers[i].label[0] ? ", " : "", hms);
        cJSON_AddStringToObject(r, key, line);
    }
    for (int i = 0; i < s_n_alarms; i++) {
        char key[24], when[32] = "after the clock is set";
        if (s_alarms[i].next) fmt_local(s_alarms[i].next, when, sizeof(when));
        snprintf(key, sizeof(key), "alarm %d", i + 1);
        snprintf(line, sizeof(line), "%02d:%02d %s%s%s (next %s)", s_alarms[i].hour, s_alarms[i].minute,
                 clk_repeat_name(s_alarms[i].repeat), s_alarms[i].label[0] ? ", " : "", s_alarms[i].label, when);
        cJSON_AddStringToObject(r, key, line);
    }
    if (s_sw.running || clk_sw_elapsed(&s_sw, now_ms()) > 0) {
        char hms[16];
        clk_format_hms(clk_sw_elapsed(&s_sw, now_ms()) / 1000, hms, sizeof(hms));
        snprintf(line, sizeof(line), "%s%s", hms, s_sw.running ? " (running)" : " (stopped)");
        cJSON_AddStringToObject(r, "stopwatch", line);
    }
    xSemaphoreGive(s_lock);
    return r;
}

// ---------------------------------------------------------------- declarations

static cJSON *fn(cJSON *decls, const char *name, const char *desc)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", name);
    cJSON_AddStringToObject(f, "description", desc);
    cJSON_AddItemToArray(decls, f);
    return f;
}

static cJSON *params(cJSON *f, const char *const *required, int nreq)
{
    cJSON *p = cJSON_AddObjectToObject(f, "parameters");
    cJSON_AddStringToObject(p, "type", "OBJECT");
    cJSON *props = cJSON_AddObjectToObject(p, "properties");
    if (nreq) cJSON_AddItemToObject(p, "required", cJSON_CreateStringArray(required, nreq));
    return props;
}

static void prop(cJSON *props, const char *name, const char *type, const char *desc)
{
    cJSON *p = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
}

void clock_tool_declarations(cJSON *decls)
{
    static const char *const req_secs[] = { "seconds" };
    cJSON *props = params(fn(decls, "set_timer", "Start a countdown timer on the board. It beeps when done, even offline."), req_secs, 1);
    prop(props, "seconds", "INTEGER", "Length in seconds (convert minutes/hours), 1 to 86400.");
    prop(props, "label", "STRING", "Optional short name, e.g. 'pasta'.");

    fn(decls, "list_timers", "Running timers with the time left on each.");

    props = params(fn(decls, "cancel_timer", "Cancel a timer."), NULL, 0);
    prop(props, "label", "STRING", "The timer's name, or 'all'. Omit if only one timer is running.");
    prop(props, "id", "INTEGER", "The timer's id from list_timers.");

    static const char *const req_time[] = { "time" };
    props = params(fn(decls, "set_alarm", "Set an alarm on the board (kept across restarts)."), req_time, 1);
    prop(props, "time", "STRING", "24-hour local time HH:MM, e.g. '07:30' or '19:05'.");
    prop(props, "repeat", "STRING", "once (default), daily, weekdays or weekends.");
    prop(props, "date", "STRING", "For a one-off alarm on a specific day: YYYY-MM-DD. Omit for the next occurrence.");
    prop(props, "label", "STRING", "Optional short name, e.g. 'gym'.");

    fn(decls, "list_alarms", "Alarms that are set, with when each goes off next.");

    props = params(fn(decls, "cancel_alarm", "Cancel an alarm."), NULL, 0);
    prop(props, "label", "STRING", "The alarm's name, or 'all'. Omit if only one alarm is set.");
    prop(props, "id", "INTEGER", "The alarm's id from list_alarms.");

    static const char *const req_action[] = { "action" };
    props = params(fn(decls, "stopwatch", "Control or read the stopwatch."), req_action, 1);
    prop(props, "action", "STRING", "start, stop, lap, reset or status.");
}
