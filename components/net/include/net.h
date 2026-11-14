// SPDX-License-Identifier: MIT
// Wi-Fi station management. Credentials are stored by the Wi-Fi driver in NVS.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t net_init(void);
/** True if credentials have been saved (via Improv or the setup page). */
bool net_has_credentials(void);
/** Connect with saved credentials; waits up to timeout_ms. */
esp_err_t net_connect(int timeout_ms);
/** Try new credentials; saves them only if the connection succeeds. */
esp_err_t net_provision(const char *ssid, const char *password, int timeout_ms);
/** Turn Wi-Fi off (battery saving between conversations). */
void net_disconnect(void);
bool net_is_connected(void);
/** Plain-language text for a Wi-Fi disconnect reason code. */
const char *net_reason_text(int reason);
/** Log a one-line plain-language Wi-Fi status (for the serial console). */
void net_log_status(void);
/** Writes e.g. "http://192.168.1.50/" into buf. */
esp_err_t net_device_url(char *buf, size_t len);
