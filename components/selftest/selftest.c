// SPDX-License-Identifier: MIT
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "audio.h"
#include "wakeword.h"
#include "prompts.h"
#include "leds.h"
#include "selftest.h"

static const char *TAG = "selftest";

#define RATE         16000
#define MAX_SECONDS  4
#define TIMEOUT_MS   8000
#define MIN_SAMPLES  (RATE / 5)          // 0.2 s counts as "heard you"

static volatile selftest_state_t s_state = SELFTEST_IDLE;
static volatile float s_seconds;

static void mic_test_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    leds_state_t before = leds_get();

    leds_set(LEDS_SPEAKING);
    prompts_say_always(PROMPT_MIC_TEST_START);
    audio_wait_idle(5000);                            // don't record our own voice

    leds_set(LEDS_LISTENING);
    int n = buf ? wakeword_capture(buf, RATE * MAX_SECONDS, TIMEOUT_MS) : 0;
    s_seconds = n > 0 ? (float)n / RATE : 0;
    ESP_LOGI(TAG, "mic test: captured %.2f s of speech", s_seconds);

    leds_set(LEDS_SPEAKING);
    if (n >= MIN_SAMPLES) {
        prompts_say_always(PROMPT_MIC_TEST_HELLO);
        audio_wait_idle(5000);
        audio_play_16k(buf, (size_t)n);
        audio_wait_idle(MAX_SECONDS * 1000 + 1000);
        s_state = SELFTEST_HEARD;
    } else {
        prompts_say_always(PROMPT_MIC_TEST_NOTHING);
        audio_wait_idle(6000);
        s_state = SELFTEST_NOTHING;
    }
    leds_set(before == LEDS_ERROR ? LEDS_OFF : before);
    free(buf);
    vTaskDelete(NULL);
}

esp_err_t selftest_mic_start(void)
{
    if (!audio_mic_ready()) return ESP_ERR_NOT_SUPPORTED;
    if (s_state == SELFTEST_RUNNING || !wakeword_is_idle()) return ESP_ERR_INVALID_STATE;
    s_state = SELFTEST_RUNNING;
    s_seconds = 0;
    if (xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 4, NULL) != pdPASS) {
        s_state = SELFTEST_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

selftest_state_t selftest_mic_state(float *seconds_heard)
{
    if (seconds_heard) *seconds_heard = s_seconds;
    return s_state;
}
