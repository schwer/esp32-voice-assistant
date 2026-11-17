// SPDX-License-Identifier: MIT
//
// Which Wi-Fi network to join: pure logic, no ESP-IDF, unit-tested on the host
// (tests/host/test_net.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NET_MAX_SAVED   5
#define NET_SSID_MAX    33      // 32 + NUL
#define NET_PASS_MAX    65      // 64 + NUL
#define NET_OPEN_MIN_RSSI (-80) // weaker open networks are rarely usable

typedef struct {
    char ssid[NET_SSID_MAX];
    char pass[NET_PASS_MAX];    // "" = open network the user saved on purpose
} net_saved_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    int rssi;
    bool open;                  // no password
} net_ap_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char pass[NET_PASS_MAX];
    bool saved;                 // one of the user's networks (trusted)
    bool open;
} net_choice_t;

/** Add or update a network, most recent first. Drops the oldest beyond NET_MAX_SAVED.
 *  Returns the new count. */
int net_saved_add(net_saved_t *list, int n, const char *ssid, const char *pass);
/** Remove a network by name. Returns the new count. */
int net_saved_remove(net_saved_t *list, int n, const char *ssid);

/**
 * Networks to try, best first: the user's saved networks that are in range
 * (strongest first), then, if allow_open, open networks in range that aren't
 * blocked (e.g. found to need a sign-in page), strongest first.
 * A saved network with a password is only matched to a secured access point, and
 * a saved open one only to an open access point. Returns the number written.
 */
int net_pick(const net_ap_t *aps, int n_aps, const net_saved_t *saved, int n_saved,
             bool allow_open, const char (*blocked)[NET_SSID_MAX], int n_blocked,
             net_choice_t *out, int max);
