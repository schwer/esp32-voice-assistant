// SPDX-License-Identifier: MIT
//
// Captive Wi-Fi setup: while the board's setup hotspot is on, every web address a
// phone tries leads to a small page for choosing a Wi-Fi network and entering its
// password (the phone shows it as "sign in to network"). It offers Wi-Fi setup only:
// the full setup page (API key, settings) isn't reachable from the open hotspot.
#pragma once
#include "esp_err.h"

/** Start the DNS responder and the setup page on the hotspot (192.168.4.1). */
esp_err_t captive_start(void);
void captive_stop(void);
