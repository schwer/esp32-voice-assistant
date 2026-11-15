// SPDX-License-Identifier: MIT
//
// Improv Wi-Fi serial protocol (https://www.improv-wifi.com/serial/): packet encoding,
// a byte-at-a-time parser, and RPC payload helpers. No ESP-IDF dependencies, so it is
// unit-tested on the host (tests/host).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { IMPROV_TYPE_STATE = 0x01, IMPROV_TYPE_ERROR = 0x02, IMPROV_TYPE_RPC = 0x03, IMPROV_TYPE_RESULT = 0x04 };
enum { IMPROV_STATE_READY = 0x02, IMPROV_STATE_PROVISIONING = 0x03, IMPROV_STATE_PROVISIONED = 0x04 };
enum { IMPROV_ERR_NONE = 0x00, IMPROV_ERR_INVALID_RPC = 0x01, IMPROV_ERR_UNKNOWN_RPC = 0x02,
       IMPROV_ERR_UNABLE_TO_CONNECT = 0x03, IMPROV_ERR_UNKNOWN = 0xFF };
enum { IMPROV_CMD_WIFI_SETTINGS = 0x01, IMPROV_CMD_GET_STATE = 0x02, IMPROV_CMD_GET_INFO = 0x03,
       IMPROV_CMD_GET_NETWORKS = 0x04,
       IMPROV_CMD_VENDOR_STATUS = 0xF0 };  // ours: [ssid, setup URL, rssi, saved ssid]

#define IMPROV_VERSION 1
/** Largest encoded packet: header(6) + version, type, length + 255 data + checksum + '\n'. */
#define IMPROV_MAX_PACKET (6 + 3 + 255 + 1 + 1)

/** Encode a packet into `out` (IMPROV_MAX_PACKET bytes). Returns its length. */
size_t improv_encode(uint8_t type, const uint8_t *data, uint8_t len, uint8_t *out);

/**
 * RPC result payload: [cmd][total length][len][string]... Strings that don't fit in
 * 255 bytes are left out. Returns the payload length.
 */
size_t improv_result_payload(uint8_t cmd, const char *const *strs, int count, uint8_t out[255]);

typedef struct {
    uint8_t buf[6 + 3 + 255 + 1];
    size_t pos;
} improv_parser_t;

typedef struct {
    uint8_t version, type, len;
    const uint8_t *data;   // points into the parser; valid until the next byte is fed
} improv_packet_t;

void improv_parser_reset(improv_parser_t *p);
/**
 * Feed one byte. Returns 1 when a packet with a valid checksum is complete (in *out),
 * -1 when a complete packet had a bad checksum, 0 otherwise. Other serial output (logs)
 * between packets is skipped.
 */
int improv_parser_feed(improv_parser_t *p, uint8_t c, improv_packet_t *out);

/**
 * Parse the payload of an RPC packet: [cmd][length][args...]. Returns false if the
 * declared length doesn't match.
 */
bool improv_rpc_split(const uint8_t *d, uint8_t len, uint8_t *cmd, const uint8_t **args, uint8_t *args_len);

/** Wi-Fi settings arguments: [ssid_len][ssid][pass_len][pass]. False if malformed or too long. */
bool improv_parse_wifi(const uint8_t *args, uint8_t len, char ssid[33], char pass[65]);
