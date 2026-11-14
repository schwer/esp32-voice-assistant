// SPDX-License-Identifier: MIT
// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/), used by the
// GitHub Pages web flasher (ESP Web Tools) to set Wi-Fi right after flashing.
#pragma once
#include "esp_err.h"

/** Start the Improv serial listener task on the console port. */
esp_err_t improv_start(void);
