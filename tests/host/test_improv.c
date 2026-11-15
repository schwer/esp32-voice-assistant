// SPDX-License-Identifier: MIT
// Improv Wi-Fi serial protocol: encoding, parsing amid log output, RPC payloads.
#include <string.h>
#include "improv_proto.h"
#include "test.h"

static int feed(improv_parser_t *p, const uint8_t *b, size_t n, improv_packet_t *out)
{
    int r = 0;
    for (size_t i = 0; i < n; i++) {
        int x = improv_parser_feed(p, b[i], out);
        if (x) r = x;
    }
    return r;
}

static void encode_then_parse_round_trips(void)
{
    uint8_t pkt[IMPROV_MAX_PACKET];
    const uint8_t data[] = { 0x03, 0x00 };                     // RPC: get device info
    size_t n = improv_encode(IMPROV_TYPE_RPC, data, 2, pkt);
    CHECK(n == 6 + 3 + 2 + 1 + 1);
    CHECK(memcmp(pkt, "IMPROV", 6) == 0 && pkt[n - 1] == '\n');

    improv_parser_t p;
    improv_parser_reset(&p);
    improv_packet_t out;
    CHECK(feed(&p, pkt, n - 1, &out) == 1);                   // newline not needed
    CHECK(out.version == 1 && out.type == IMPROV_TYPE_RPC && out.len == 2);
    CHECK(out.data[0] == 0x03 && out.data[1] == 0x00);
}

static void parser_skips_log_output_and_resyncs(void)
{
    uint8_t pkt[IMPROV_MAX_PACKET];
    const uint8_t data[] = { IMPROV_STATE_READY };
    size_t n = improv_encode(IMPROV_TYPE_STATE, data, 1, pkt);
    uint8_t stream[512];
    const char *logs = "I (123) main: IMPRO hello\nW (5) net: IMPIMPROV";   // partial headers
    size_t ln = strlen(logs);
    memcpy(stream, logs, ln);
    memcpy(stream + ln, pkt, n);
    improv_parser_t p;
    improv_parser_reset(&p);
    improv_packet_t out;
    // "IMPIMPROV" in the logs looks like a header; the real packet right after it
    // must still be found (regression: the false header used to swallow it).
    CHECK(feed(&p, stream, ln + n, &out) == 1);
    CHECK(out.type == IMPROV_TYPE_STATE && out.data[0] == IMPROV_STATE_READY);
    // Feed a clean packet after the noise: always found.
    improv_parser_reset(&p);
    CHECK(feed(&p, (const uint8_t *)"noise noise ", 12, &out) == 0);
    CHECK(feed(&p, pkt, n, &out) == 1);
    CHECK(out.type == IMPROV_TYPE_STATE && out.data[0] == IMPROV_STATE_READY);
}

static void parser_rejects_bad_checksum(void)
{
    uint8_t pkt[IMPROV_MAX_PACKET];
    const uint8_t data[] = { 1, 2, 3 };
    size_t n = improv_encode(IMPROV_TYPE_RPC, data, 3, pkt);
    pkt[n - 2] ^= 0xFF;                                        // corrupt checksum
    improv_parser_t p;
    improv_parser_reset(&p);
    improv_packet_t out;
    CHECK(feed(&p, pkt, n, &out) == -1);
    // and recovers for the next packet
    n = improv_encode(IMPROV_TYPE_RPC, data, 3, pkt);
    CHECK(feed(&p, pkt, n, &out) == 1);
}

static void result_payload_layout_and_truncation(void)
{
    uint8_t buf[255];
    const char *s[] = { "ESP32 Voice Assistant", "abc1234", "ESP32-S3", "assistant" };
    size_t n = improv_result_payload(IMPROV_CMD_GET_INFO, s, 4, buf);
    CHECK(buf[0] == IMPROV_CMD_GET_INFO);
    CHECK(buf[1] == n - 2);
    CHECK(buf[2] == strlen(s[0]) && memcmp(&buf[3], s[0], buf[2]) == 0);

    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[199] = 0;
    const char *two[] = { big, big };                          // 2 x 199 bytes can't both fit
    n = improv_result_payload(IMPROV_CMD_GET_INFO, two, 2, buf);
    CHECK(n <= 255 && buf[1] == n - 2 && n == 2 + 1 + 199);

    n = improv_result_payload(IMPROV_CMD_GET_NETWORKS, NULL, 0, buf);   // end of scan list
    CHECK(n == 2 && buf[1] == 0);
}

static void rpc_split_checks_declared_length(void)
{
    uint8_t cmd, alen;
    const uint8_t *args;
    const uint8_t ok[] = { 0x01, 0x02, 0xAA, 0xBB };
    CHECK(improv_rpc_split(ok, 4, &cmd, &args, &alen) && cmd == 1 && alen == 2 && args[0] == 0xAA);
    const uint8_t bad[] = { 0x01, 0x05, 0xAA };
    CHECK(!improv_rpc_split(bad, 3, &cmd, &args, &alen));
    CHECK(!improv_rpc_split(ok, 1, &cmd, &args, &alen));
}

static void wifi_settings_parsing(void)
{
    char ssid[33], pass[65];
    const uint8_t ok[] = { 4, 'h', 'o', 'm', 'e', 3, 'p', 'w', 'd' };
    CHECK(improv_parse_wifi(ok, sizeof(ok), ssid, pass));
    CHECK(strcmp(ssid, "home") == 0 && strcmp(pass, "pwd") == 0);

    const uint8_t open_net[] = { 3, 'c', 'a', 'f', 0 };
    CHECK(improv_parse_wifi(open_net, sizeof(open_net), ssid, pass) && pass[0] == 0);

    const uint8_t truncated[] = { 4, 'h', 'o', 'm', 'e', 9, 'p' };   // claims 9-byte password
    CHECK(!improv_parse_wifi(truncated, sizeof(truncated), ssid, pass));

    uint8_t long_ssid[40] = { 33 };                                   // SSIDs are max 32 bytes
    CHECK(!improv_parse_wifi(long_ssid, sizeof(long_ssid), ssid, pass));

    const uint8_t empty[] = { 0 };
    CHECK(!improv_parse_wifi(empty, 1, ssid, pass));
}

int main(void)
{
    RUN(encode_then_parse_round_trips);
    RUN(parser_skips_log_output_and_resyncs);
    RUN(parser_rejects_bad_checksum);
    RUN(result_payload_layout_and_truncation);
    RUN(rpc_split_checks_declared_length);
    RUN(wifi_settings_parsing);
    TEST_MAIN_END("improv");
}
