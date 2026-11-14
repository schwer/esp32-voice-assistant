// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "esp_err.h"

/** Start the local setup server (mDNS: assistant.local). Gated by a button press + PIN. */
esp_err_t setup_web_start(void);
esp_err_t setup_web_stop(void);
/** esp_timer time of the last page request (someone is using the pages). */
int64_t setup_web_last_activity_us(void);
