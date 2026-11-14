// SPDX-License-Identifier: MIT
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "led_strip.h"
#include "board.h"
#include "leds.h"

static const char *TAG = "leds";

#define FRAME_MS    33
#define BRIGHTNESS  48          // of 255; the ring is bright and costs battery
#define PREVIEW_MS  4000

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile leds_state_t s_state = LEDS_OFF;
static volatile int s_event = -1;          // active one-shot event, -1 = none
static volatile bool s_event_restart;
static TimerHandle_t s_preview_timer;
static leds_state_t s_preview_restore;
static volatile bool s_previewing;
static leds_change_cb_t s_on_change;

void leds_on_change(leds_change_cb_t cb) { s_on_change = cb; }

static const struct { const char *name, *desc; } STATES[LEDS_STATE_COUNT] = {
    [LEDS_OFF]          = { "off",          "Off: idle, waiting for \"Jarvis\"" },
    [LEDS_CONNECTING]   = { "connecting",   "Blue pulse: starting up or connecting" },
    [LEDS_LISTENING]    = { "listening",    "Rainbow cycle: ready for your voice" },
    [LEDS_THINKING]     = { "thinking",     "Purple spinner: working on your request" },
    [LEDS_SPEAKING]     = { "speaking",     "Soft white glow: assistant is talking" },
    [LEDS_SETUP_NEEDED] = { "setup_needed", "Amber blink every few seconds: Wi-Fi or API key missing" },
    [LEDS_REMINDER]     = { "reminder",     "Amber breathing: a reminder is due" },
    [LEDS_ERROR]        = { "error",        "3 red blinks: something went wrong (see diagnostics)" },
};

static const struct { const char *name, *desc; } EVENTS[LEDS_EVENT_COUNT] = {
    [LEDS_EVENT_WAKE]    = { "wake",    "Quick white flash: heard \"Jarvis\"" },
    [LEDS_EVENT_SAVED]   = { "saved",   "2 green blinks: saved" },
    [LEDS_EVENT_DELETED] = { "deleted", "Orange sweep: deleted" },
};

const char *leds_state_name(leds_state_t s)        { return s < LEDS_STATE_COUNT ? STATES[s].name : ""; }
const char *leds_state_description(leds_state_t s) { return s < LEDS_STATE_COUNT ? STATES[s].desc : ""; }
const char *leds_event_name(leds_event_t e)        { return e < LEDS_EVENT_COUNT ? EVENTS[e].name : ""; }
const char *leds_event_description(leds_event_t e) { return e < LEDS_EVENT_COUNT ? EVENTS[e].desc : ""; }

// ---------- drawing ----------

static uint8_t scale(float v)
{
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (uint8_t)(v * BRIGHTNESS);
}

static void px(int i, float r, float g, float b)
{
    led_strip_set_pixel(s_strip, i, scale(r), scale(g), scale(b));
}

static void fill(float r, float g, float b)
{
    for (int i = 0; i < BOARD_LED_COUNT; i++) px(i, r, g, b);
}

/** Hue (0..1) to RGB at full saturation/value. */
static void hue_rgb(float h, float *r, float *g, float *b)
{
    float x = h * 6.0f;
    int i = (int)x % 6;
    float f = x - floorf(x);
    float q = 1.0f - f;
    switch (i) {
    case 0: *r = 1; *g = f; *b = 0; break;
    case 1: *r = q; *g = 1; *b = 0; break;
    case 2: *r = 0; *g = 1; *b = f; break;
    case 3: *r = 0; *g = q; *b = 1; break;
    case 4: *r = f; *g = 0; *b = 1; break;
    default: *r = 1; *g = 0; *b = q; break;
    }
}

static float breathe(float t, float period, float lo)
{
    return lo + (1.0f - lo) * (0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI / period));
}

/** Comet: bright head with a fading tail, rotating once per period. */
static void spinner(float t, float period, float r, float g, float b)
{
    float head = fmodf(t / period, 1.0f) * BOARD_LED_COUNT;
    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        float d = fmodf(head - i + BOARD_LED_COUNT, BOARD_LED_COUNT);   // distance behind head
        float v = d < 3.0f ? 1.0f - d / 3.0f : 0.06f;
        px(i, r * v, g * v, b * v);
    }
}

/** Draw a state frame. Returns false when the state is finished (error) or static-dark. */
static bool draw_state(leds_state_t st, float t)
{
    switch (st) {
    case LEDS_OFF:
        led_strip_clear(s_strip);
        return false;
    case LEDS_CONNECTING:
        fill(0, 0.35f * breathe(t, 1.2f, 0.15f), breathe(t, 1.2f, 0.15f));
        return true;
    case LEDS_LISTENING:
        for (int i = 0; i < BOARD_LED_COUNT; i++) {
            float r, g, b;
            hue_rgb(fmodf((float)i / BOARD_LED_COUNT + t / 1.5f, 1.0f), &r, &g, &b);
            px(i, r, g, b);
        }
        return true;
    case LEDS_THINKING:
        spinner(t, 0.8f, 0.6f, 0.1f, 1.0f);
        return true;
    case LEDS_SPEAKING: {
        float v = 0.55f + 0.25f * sinf(t * 2.0f * (float)M_PI / 2.0f);
        fill(v, v, v * 0.9f);
        return true;
    }
    case LEDS_SETUP_NEEDED: {
        float p = fmodf(t, 4.0f);
        float v = p < 0.4f ? sinf(p / 0.4f * (float)M_PI) : 0;
        fill(v, 0.55f * v, 0);
        return true;
    }
    case LEDS_REMINDER: {
        float v = breathe(t, 2.0f, 0.1f);
        fill(v, 0.55f * v, 0);
        return true;
    }
    case LEDS_ERROR: {
        bool on = t < 1.5f && ((int)(t / 0.25f) % 2) == 0;
        fill(on ? 1.0f : 0, 0, 0);
        return t < 1.5f;
    }
    default:
        return false;
    }
}

/** Draw an event frame. Returns false when the event has finished. */
static bool draw_event(leds_event_t ev, float t)
{
    switch (ev) {
    case LEDS_EVENT_WAKE: {
        if (t > 0.35f) return false;
        float v = 1.0f - t / 0.35f;
        fill(v, v, v);
        return true;
    }
    case LEDS_EVENT_SAVED: {
        if (t > 0.9f) return false;
        bool on = t < 0.2f || (t > 0.4f && t < 0.6f);
        fill(0, on ? 1.0f : 0, on ? 0.2f : 0);
        return true;
    }
    case LEDS_EVENT_DELETED: {
        if (t > 0.8f) return false;
        int lit = (int)(t / 0.8f * BOARD_LED_COUNT);
        for (int i = 0; i < BOARD_LED_COUNT; i++) {
            bool on = i >= lit;
            px(i, on ? 1.0f : 0, on ? 0.35f : 0, 0);
        }
        return true;
    }
    default:
        return false;
    }
}

static void leds_task(void *arg)
{
    uint32_t state_frame = 0, event_frame = 0;
    leds_state_t last_state = (leds_state_t)-1;

    for (;;) {
        leds_state_t st = s_state;
        if (st != last_state) {
            state_frame = 0;
            last_state = st;
        }
        if (s_event_restart) {
            s_event_restart = false;
            event_frame = 0;
        }

        bool animating;
        int ev = s_event;
        if (ev >= 0) {
            animating = draw_event((leds_event_t)ev, event_frame++ * FRAME_MS / 1000.0f);
            if (!animating) {
                s_event = -1;
                animating = true;             // redraw the state next frame
            }
        } else {
            animating = draw_state(st, state_frame++ * FRAME_MS / 1000.0f);
            if (!animating && st == LEDS_ERROR) {
                s_state = LEDS_OFF;
            }
        }
        led_strip_refresh(s_strip);

        // Sleep until the next frame, or indefinitely when dark and idle.
        ulTaskNotifyTake(pdTRUE, animating ? pdMS_TO_TICKS(FRAME_MS) : portMAX_DELAY);
    }
}

// ---------- API ----------

esp_err_t leds_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = BOARD_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = BOARD_LED_COLOR_FORMAT,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED ring init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    xTaskCreate(leds_task, "leds", 3072, NULL, 2, &s_task);
    return ESP_OK;
}

void leds_set(leds_state_t state)
{
    if (state >= LEDS_STATE_COUNT) return;
    leds_state_t from = leds_get();
    if (s_previewing) {
        s_preview_restore = state;     // apply once the preview ends
    } else {
        s_state = state;
        if (s_task) xTaskNotifyGive(s_task);
    }
    if (from != state && s_on_change) {
        s_on_change(from, state);
    }
}

leds_state_t leds_get(void)
{
    return s_previewing ? s_preview_restore : s_state;
}

void leds_event(leds_event_t event)
{
    if (event >= LEDS_EVENT_COUNT) return;
    s_event = event;
    s_event_restart = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void preview_done(TimerHandle_t t)
{
    s_previewing = false;
    s_state = s_preview_restore;       // back to the real state, silently
    if (s_task) xTaskNotifyGive(s_task);
}

bool leds_preview(const char *name)
{
    if (!s_preview_timer) {
        s_preview_timer = xTimerCreate("led_preview", pdMS_TO_TICKS(PREVIEW_MS), pdFALSE, NULL, preview_done);
    }
    for (int i = 0; i < LEDS_EVENT_COUNT; i++) {
        if (strcmp(name, EVENTS[i].name) == 0) {
            leds_event((leds_event_t)i);
            return true;
        }
    }
    for (int i = 0; i < LEDS_STATE_COUNT; i++) {
        if (strcmp(name, STATES[i].name) == 0) {
            if (!s_previewing) {
                s_preview_restore = s_state;
                s_previewing = true;
            }
            s_state = (leds_state_t)i;
            if (s_task) xTaskNotifyGive(s_task);
            xTimerReset(s_preview_timer, 0);
            return true;
        }
    }
    return false;
}
