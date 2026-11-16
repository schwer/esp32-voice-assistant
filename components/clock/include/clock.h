// SPDX-License-Identifier: MIT
//
// Timers, alarms and a stopwatch, all running on the board (they work offline).
// Alarms are kept on flash; timers and the stopwatch last until a restart.
// When one goes off the board says so and beeps until stopped ("Jarvis" or the
// diagnostics page) or it times out.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

typedef enum { CLOCK_RING_TIMER, CLOCK_RING_ALARM } clock_ring_t;

/** Ringing started (true) or stopped (false). Called from the clock task; keep it short. */
typedef void (*clock_ring_cb_t)(bool ringing, clock_ring_t kind, const char *label);

esp_err_t clock_init(clock_ring_cb_t on_ring);
bool clock_ringing(void);
void clock_stop_ringing(void);

/** Gemini tools: set_timer, list_timers, cancel_timer, set_alarm, list_alarms,
 *  cancel_alarm, stopwatch. Returns NULL if `name` isn't one of them. */
cJSON *clock_tool(const char *name, const cJSON *args);
/** Function declarations for those tools (added to the tools list). */
void clock_tool_declarations(cJSON *decls);
/** Everything active, for the diagnostics page. */
cJSON *clock_snapshot(void);
