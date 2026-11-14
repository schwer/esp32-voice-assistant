// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

/** Start the local setup server (mDNS: assistant.local). Gated by a button press + PIN. */
esp_err_t setup_web_start(void);
esp_err_t setup_web_stop(void);
