// SPDX-License-Identifier: MIT
// Wi-Fi: the user's saved networks (home, phone hotspot, ...) and, if allowed, open
// networks with no password. See net.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "net_logic.h"

esp_err_t net_init(void);
/** True if at least one network has been saved (via the install page or the setup page). */
bool net_has_credentials(void);
/** Connect with saved credentials; waits up to timeout_ms. */
esp_err_t net_connect(int timeout_ms);
/** Join a new network now; adds it to the saved networks only if that works. */
esp_err_t net_provision(const char *ssid, const char *password, int timeout_ms);
/** Save a network without joining it now (e.g. a phone hotspot that's off). */
esp_err_t net_add_network(const char *ssid, const char *password);
esp_err_t net_forget(const char *ssid);
/** Saved network names, most recent first. Returns the count. */
int net_saved_networks(char (*ssids)[NET_SSID_MAX], int max);
/** Allow joining open networks (no password) when none of ours is around. */
void net_set_allow_open(bool allow);
/** Connected to an open network that isn't one of the saved ones. */
bool net_on_public_network(void);
/** Called (from the Wi-Fi task) when we move between our networks and public ones. */
typedef void (*net_trust_cb_t)(bool public_network);
void net_on_trust_change(net_trust_cb_t cb);
/** Turn Wi-Fi off (battery saving between conversations). */
void net_disconnect(void);
bool net_is_connected(void);
/** Scan nearby networks (strongest first, duplicates removed). Returns count. */
int net_scan(wifi_ap_record_t *out, int max);
/** SSID of the connected network, or "" when offline. */
void net_current_ssid(char *buf, size_t len);
/** The most recently saved network, or "". */
void net_saved_ssid(char *buf, size_t len);
/** Signal strength of the current connection in dBm (0 when offline). */
int net_rssi(void);
/** Idle: let the radio doze between beacons. Conversation: stay fully awake (smooth audio). */
void net_power_save(bool idle);
/** Plain-language text for a Wi-Fi disconnect reason code. */
const char *net_reason_text(int reason);
/** Log a one-line plain-language Wi-Fi status (for the serial console). */
void net_log_status(void);
/** Network sockets open right now, and the most there can be (diagnostics). */
int net_sockets_in_use(int *max);
/** Writes e.g. "http://192.168.1.50/" into buf. */
esp_err_t net_device_url(char *buf, size_t len);
