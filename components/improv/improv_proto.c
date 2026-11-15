// SPDX-License-Identifier: MIT
#include <string.h>
#include "improv_proto.h"

static const uint8_t HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

size_t improv_encode(uint8_t type, const uint8_t *data, uint8_t len, uint8_t *out)
{
    size_t n = 0;
    memcpy(out, HEADER, 6);
    n = 6;
    out[n++] = IMPROV_VERSION;
    out[n++] = type;
    out[n++] = len;
    if (len) memcpy(&out[n], data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += out[i];
    out[n++] = sum;
    out[n++] = '\n';
    return n;
}

size_t improv_result_payload(uint8_t cmd, const char *const *strs, int count, uint8_t out[255])
{
    size_t n = 2;
    for (int i = 0; i < count; i++) {
        size_t l = strlen(strs[i]);
        if (n + 1 + l > 255) break;
        out[n++] = (uint8_t)l;
        memcpy(&out[n], strs[i], l);
        n += l;
    }
    out[0] = cmd;
    out[1] = (uint8_t)(n - 2);
    return n;
}

void improv_parser_reset(improv_parser_t *p)
{
    p->pos = 0;
}

int improv_parser_feed(improv_parser_t *p, uint8_t c, improv_packet_t *out)
{
    // Match the header byte by byte; resync on mismatch.
    if (p->pos < 6) {
        if (c == HEADER[p->pos]) {
            p->buf[p->pos++] = c;
        } else {
            p->pos = (c == HEADER[0]) ? 1 : 0;
            if (p->pos) p->buf[0] = c;
        }
        return 0;
    }
    p->buf[p->pos++] = c;
    if (p->pos < 9) return 0;                              // version, type, length
    const uint8_t len = p->buf[8];
    if (p->pos < 9u + len + 1u) return 0;                  // data + checksum
    uint8_t sum = 0;
    for (size_t i = 0; i < 9u + len; i++) sum += p->buf[i];
    p->pos = 0;
    if (sum != p->buf[9 + len]) return -1;
    out->version = p->buf[6];
    out->type = p->buf[7];
    out->len = len;
    out->data = &p->buf[9];
    return 1;
}

bool improv_rpc_split(const uint8_t *d, uint8_t len, uint8_t *cmd, const uint8_t **args, uint8_t *args_len)
{
    if (len < 2 || d[1] != len - 2) return false;
    *cmd = d[0];
    *args = &d[2];
    *args_len = d[1];
    return true;
}

bool improv_parse_wifi(const uint8_t *a, uint8_t len, char ssid[33], char pass[65])
{
    if (len < 2) return false;
    uint8_t ssid_len = a[0];
    if (1 + ssid_len + 1 > len || ssid_len > 32) return false;
    uint8_t pass_len = a[1 + ssid_len];
    if (1 + ssid_len + 1 + pass_len > len || pass_len > 64) return false;
    memcpy(ssid, &a[1], ssid_len);
    ssid[ssid_len] = 0;
    memcpy(pass, &a[2 + ssid_len], pass_len);
    pass[pass_len] = 0;
    return true;
}
