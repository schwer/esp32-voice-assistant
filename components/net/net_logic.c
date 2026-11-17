// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "net_logic.h"

static void copy(char *dst, const char *src, size_t len)
{
    snprintf(dst, len, "%s", src ? src : "");
}

int net_saved_add(net_saved_t *list, int n, const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return n;
    n = net_saved_remove(list, n, ssid);
    if (n >= NET_MAX_SAVED) n = NET_MAX_SAVED - 1;           // forget the oldest
    memmove(&list[1], &list[0], (size_t)n * sizeof(*list));
    copy(list[0].ssid, ssid, sizeof(list[0].ssid));
    copy(list[0].pass, pass, sizeof(list[0].pass));
    return n + 1;
}

int net_saved_remove(net_saved_t *list, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            memmove(&list[i], &list[i + 1], (size_t)(n - i - 1) * sizeof(*list));
            return n - 1;
        }
    }
    return n;
}

static bool listed(const char *ssid, const char (*names)[NET_SSID_MAX], int n)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(ssid, names[i]) == 0) return true;
    }
    return false;
}

static int saved_index(const net_saved_t *saved, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(saved[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static bool chosen(const net_choice_t *out, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].ssid, ssid) == 0) return true;
    }
    return false;
}

int net_pick(const net_ap_t *aps, int n_aps, const net_saved_t *saved, int n_saved,
             bool allow_open, const char (*blocked)[NET_SSID_MAX], int n_blocked,
             net_choice_t *out, int max)
{
    // Strongest first, without changing the caller's array: repeated selection.
    int order[64];
    int n = n_aps < 64 ? n_aps : 64;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (aps[order[j]].rssi > aps[order[i]].rssi) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    int count = 0;
    for (int pass = 0; pass < 2; pass++) {               // 0: saved networks, 1: open ones
        for (int k = 0; k < n && count < max; k++) {
            const net_ap_t *ap = &aps[order[k]];
            if (!ap->ssid[0] || chosen(out, count, ap->ssid)) continue;
            int s = saved_index(saved, n_saved, ap->ssid);
            if (pass == 0) {
                if (s < 0 || (saved[s].pass[0] != 0) == ap->open) continue;
                copy(out[count].ssid, ap->ssid, sizeof(out[count].ssid));
                copy(out[count].pass, saved[s].pass, sizeof(out[count].pass));
                out[count].saved = true;
                out[count].open = ap->open;
                count++;
            } else {
                if (!allow_open || !ap->open || s >= 0 || ap->rssi < NET_OPEN_MIN_RSSI ||
                    listed(ap->ssid, blocked, n_blocked)) continue;
                copy(out[count].ssid, ap->ssid, sizeof(out[count].ssid));
                out[count].pass[0] = 0;
                out[count].saved = false;
                out[count].open = true;
                count++;
            }
        }
    }
    return count;
}
