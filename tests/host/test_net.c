// SPDX-License-Identifier: MIT
// Choosing a Wi-Fi network (components/net/net_logic.c).
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "net_logic.h"

static void saved_list_is_most_recent_first_and_bounded(void)
{
    net_saved_t l[NET_MAX_SAVED];
    int n = 0;
    n = net_saved_add(l, n, "Home", "pw1");
    n = net_saved_add(l, n, "Phone", "pw2");
    CHECK(n == 2 && !strcmp(l[0].ssid, "Phone") && !strcmp(l[1].ssid, "Home"));
    n = net_saved_add(l, n, "Home", "newpw");             // update moves it to the front
    CHECK(n == 2 && !strcmp(l[0].ssid, "Home") && !strcmp(l[0].pass, "newpw"));
    n = net_saved_add(l, n, "", "x");                     // ignored
    CHECK(n == 2);
    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "N%d", i);
        n = net_saved_add(l, n, name, "p");
    }
    CHECK(n == NET_MAX_SAVED && !strcmp(l[0].ssid, "N9") && !strcmp(l[NET_MAX_SAVED - 1].ssid, "N5"));
    n = net_saved_remove(l, n, "N7");
    CHECK(n == NET_MAX_SAVED - 1 && !strcmp(l[2].ssid, "N6"));
    CHECK(net_saved_remove(l, n, "nope") == n);
    // Long names and passwords are cut, never overflow.
    char long_ssid[80], long_pass[100];
    memset(long_ssid, 'a', 79); long_ssid[79] = 0;
    memset(long_pass, 'b', 99); long_pass[99] = 0;
    n = net_saved_add(l, n, long_ssid, long_pass);
    CHECK(strlen(l[0].ssid) == NET_SSID_MAX - 1 && strlen(l[0].pass) == NET_PASS_MAX - 1);
}

static void saved_networks_come_first(void)
{
    net_saved_t saved[] = { { "Home", "pw" }, { "Phone", "pw2" } };
    net_ap_t aps[] = { { "Cafe", -40, true }, { "Phone", -70, false }, { "Home", -60, false },
                       { "Neighbour", -50, false } };
    net_choice_t out[8];
    int n = net_pick(aps, 4, saved, 2, true, NULL, 0, out, 8);
    CHECK(n == 3);
    CHECK(!strcmp(out[0].ssid, "Home") && out[0].saved && !strcmp(out[0].pass, "pw"));
    CHECK(!strcmp(out[1].ssid, "Phone") && out[1].saved);
    CHECK(!strcmp(out[2].ssid, "Cafe") && !out[2].saved && out[2].open && !out[2].pass[0]);
}

static void open_networks_only_when_allowed(void)
{
    net_ap_t aps[] = { { "Cafe", -40, true }, { "Library", -55, true }, { "Secured", -30, false } };
    net_choice_t out[8];
    CHECK(net_pick(aps, 3, NULL, 0, false, NULL, 0, out, 8) == 0);
    int n = net_pick(aps, 3, NULL, 0, true, NULL, 0, out, 8);
    CHECK(n == 2 && !strcmp(out[0].ssid, "Cafe") && !strcmp(out[1].ssid, "Library"));
}

static void blocked_weak_and_mismatched_are_skipped(void)
{
    const char blocked[][NET_SSID_MAX] = { "Airport" };
    net_saved_t saved[] = { { "Home", "pw" }, { "Shed", "" } };
    net_ap_t aps[] = { { "Airport", -40, true },        // needs a sign-in page
                       { "Far away", -88, true },        // too weak
                       { "Home", -50, true },            // same name, but open: not our network
                       { "Shed", -60, false },           // saved as open, now secured: skip
                       { "Shed", -65, true },            // the open one we saved
                       { "", -30, true } };              // hidden
    net_choice_t out[8];
    int n = net_pick(aps, 6, saved, 2, true, blocked, 1, out, 8);
    CHECK(n == 1 && !strcmp(out[0].ssid, "Shed") && out[0].saved && out[0].open);
}

static void duplicates_and_max_are_respected(void)
{
    net_saved_t saved[] = { { "Home", "pw" } };
    net_ap_t aps[] = { { "Home", -70, false }, { "Home", -40, false }, { "A", -50, true }, { "B", -51, true } };
    net_choice_t out[2];
    int n = net_pick(aps, 4, saved, 1, true, NULL, 0, out, 2);
    CHECK(n == 2 && !strcmp(out[0].ssid, "Home") && !strcmp(out[1].ssid, "A"));
    CHECK(net_pick(NULL, 0, saved, 1, true, NULL, 0, out, 2) == 0);
}

int main(void)
{
    RUN(saved_list_is_most_recent_first_and_bounded);
    RUN(saved_networks_come_first);
    RUN(open_networks_only_when_allowed);
    RUN(blocked_weak_and_mismatched_are_skipped);
    RUN(duplicates_and_max_are_respected);
    TEST_MAIN_END("net");
}
