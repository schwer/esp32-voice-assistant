// SPDX-License-Identifier: MIT
#include <math.h>
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

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile leds_state_t s_state = LEDS_OFF;

static uint8_t scale(float v) { return (uint8_t)(v * BRIGHTNESS); }

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

static void fill(float r, float g, float b)
{
    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, scale(r), scale(g), scale(b));
    }
}

static void leds_task(void *arg)
{
    uint32_t frame = 0;
    leds_state_t last = (leds_state_t)-1;

    for (;;) {
        leds_state_t st = s_state;
        if (st != last) {
            frame = 0;
            last = st;
        }
        const float t = frame * FRAME_MS / 1000.0f;

        switch (st) {
        case LEDS_OFF:
            led_strip_clear(s_strip);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    // sleep until state changes
            continue;

        case LEDS_CONNECTING: {
            float v = 0.15f + 0.85f * (0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI / 1.2f));
            fill(0, 0.35f * v, v);
            break;
        }

        case LEDS_LISTENING:
            // Rainbow rotating around the ring, one full turn every ~1.5 s.
            for (int i = 0; i < BOARD_LED_COUNT; i++) {
                float h = fmodf((float)i / BOARD_LED_COUNT + t / 1.5f, 1.0f);
                float r, g, b;
                hue_rgb(h, &r, &g, &b);
                led_strip_set_pixel(s_strip, i, scale(r), scale(g), scale(b));
            }
            break;

        case LEDS_SPEAKING: {
            float v = 0.55f + 0.25f * sinf(t * 2.0f * (float)M_PI / 2.0f);
            fill(v, v, v * 0.9f);
            break;
        }

        case LEDS_ERROR: {
            bool on = ((int)(t / 0.25f) % 2) == 0;
            if (t > 1.5f) {
                s_state = LEDS_OFF;
            }
            fill(on ? 1.0f : 0, 0, 0);
            break;
        }
        }
        led_strip_refresh(s_strip);
        frame++;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FRAME_MS));
    }
}

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
    s_state = state;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void test_done(TimerHandle_t t)
{
    leds_set(LEDS_OFF);
}

void leds_test(void)
{
    static TimerHandle_t timer;
    if (!timer) {
        timer = xTimerCreate("led_test", pdMS_TO_TICKS(3000), pdFALSE, NULL, test_done);
    }
    leds_set(LEDS_LISTENING);
    xTimerReset(timer, 0);
}
