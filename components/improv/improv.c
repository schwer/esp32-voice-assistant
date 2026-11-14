// SPDX-License-Identifier: MIT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "net.h"
#include "improv.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#define IMPROV_UART UART_NUM_0
#endif

static const char *TAG = "improv";

enum { TYPE_STATE = 0x01, TYPE_ERROR = 0x02, TYPE_RPC = 0x03, TYPE_RPC_RESULT = 0x04 };
enum { STATE_READY = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum { ERR_NONE = 0x00, ERR_INVALID_RPC = 0x01, ERR_UNKNOWN_RPC = 0x02,
       ERR_UNABLE_TO_CONNECT = 0x03, ERR_UNKNOWN = 0xFF };
enum { CMD_WIFI_SETTINGS = 0x01, CMD_GET_STATE = 0x02, CMD_GET_INFO = 0x03,
       CMD_GET_NETWORKS = 0x04 };

static const uint8_t HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
#define VERSION 1

static void port_write(const uint8_t *data, size_t len)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#else
    uart_write_bytes(IMPROV_UART, data, len);
#endif
}

static int port_read(uint8_t *buf, size_t len)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(100));
#else
    return uart_read_bytes(IMPROV_UART, buf, len, pdMS_TO_TICKS(100));
#endif
}

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t pkt[6 + 3 + 255 + 1 + 1];
    size_t n = 0;
    memcpy(pkt, HEADER, 6);
    n = 6;
    pkt[n++] = VERSION;
    pkt[n++] = type;
    pkt[n++] = len;
    memcpy(&pkt[n], data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += pkt[i];
    }
    pkt[n++] = sum;
    pkt[n++] = '\n';
    port_write(pkt, n);
}

static void send_state(uint8_t state) { send_packet(TYPE_STATE, &state, 1); }
static void send_error(uint8_t err)   { send_packet(TYPE_ERROR, &err, 1); }

/** RPC result: [cmd][total len][len][str]... */
static void send_result(uint8_t cmd, const char *const *strs, int count)
{
    uint8_t buf[255];
    size_t n = 2;
    for (int i = 0; i < count; i++) {
        size_t l = strlen(strs[i]);
        if (n + 1 + l > sizeof(buf)) {
            break;
        }
        buf[n++] = (uint8_t)l;
        memcpy(&buf[n], strs[i], l);
        n += l;
    }
    buf[0] = cmd;
    buf[1] = (uint8_t)(n - 2);
    send_packet(TYPE_RPC_RESULT, buf, (uint8_t)n);
}

static void send_url(uint8_t cmd)
{
    char url[64];
    if (net_device_url(url, sizeof(url)) == ESP_OK) {
        const char *s[] = { url };
        send_result(cmd, s, 1);
    } else {
        send_result(cmd, NULL, 0);
    }
}

static void handle_rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] != len - 2) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    const uint8_t cmd = d[0];
    switch (cmd) {
    case CMD_GET_STATE:
        if (net_is_connected()) {
            send_state(STATE_PROVISIONED);
            send_url(cmd);
        } else {
            send_state(STATE_READY);
        }
        break;

    case CMD_GET_INFO: {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *s[] = { "ESP32 Voice Assistant", app->version, "ESP32-S3", "assistant" };
        send_result(cmd, s, 4);
        break;
    }

    case CMD_WIFI_SETTINGS: {
        // [ssid_len][ssid][pass_len][pass]
        const uint8_t *p = &d[2];
        uint8_t ssid_len = p[0];
        if (2 + 1 + ssid_len + 1 > len) {
            send_error(ERR_INVALID_RPC);
            return;
        }
        uint8_t pass_len = p[1 + ssid_len];
        if (2 + 1 + ssid_len + 1 + pass_len > len || ssid_len > 32 || pass_len > 64) {
            send_error(ERR_INVALID_RPC);
            return;
        }
        char ssid[33] = {0}, pass[65] = {0};
        memcpy(ssid, &p[1], ssid_len);
        memcpy(pass, &p[2 + ssid_len], pass_len);

        send_error(ERR_NONE);
        send_state(STATE_PROVISIONING);
        if (net_provision(ssid, pass, 15000) == ESP_OK) {
            ESP_LOGI(TAG, "provisioned '%s'", ssid);
            send_state(STATE_PROVISIONED);
            send_url(cmd);
        } else {
            send_error(ERR_UNABLE_TO_CONNECT);
            send_state(STATE_READY);
        }
        break;
    }

    default:
        send_error(ERR_UNKNOWN_RPC);
        break;
    }
}

static void improv_task(void *arg)
{
    uint8_t buf[6 + 3 + 255 + 1];
    size_t pos = 0;
    uint8_t c;

    for (;;) {
        if (port_read(&c, 1) != 1) {
            continue;
        }
        // Match the header byte by byte; resync on mismatch.
        if (pos < 6) {
            if (c == HEADER[pos]) {
                buf[pos++] = c;
            } else {
                pos = (c == HEADER[0]) ? 1 : 0;
                if (pos) buf[0] = c;
            }
            continue;
        }
        buf[pos++] = c;
        if (pos < 9) {
            continue;                                   // version, type, length
        }
        const uint8_t len = buf[8];
        if (pos < 9u + len + 1u) {
            continue;                                   // data + checksum
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < 9u + len; i++) {
            sum += buf[i];
        }
        if (buf[6] == VERSION && sum == buf[9 + len] && buf[7] == TYPE_RPC) {
            handle_rpc(&buf[9], len);
        } else if (sum != buf[9 + len]) {
            send_error(ERR_INVALID_RPC);
        }
        pos = 0;
    }
}

esp_err_t improv_start(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
#else
    esp_err_t err = uart_driver_install(IMPROV_UART, 1024, 0, 0, NULL, 0);
#endif
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    xTaskCreate(improv_task, "improv", 4096, NULL, 3, NULL);
    return ESP_OK;
}
