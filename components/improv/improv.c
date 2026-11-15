// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "net.h"
#include "improv.h"
#include "improv_proto.h"

#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"

// Boards wire USB-C either to a USB-UART bridge (-> UART0) or straight to the
// ESP32-S3's built-in USB Serial/JTAG. We listen on both and reply on whichever
// port a request arrived on, so users never have to know which one their board uses.
typedef enum { PORT_UART0, PORT_USB_JTAG } port_t;

static const char *TAG = "improv";

// Protocol constants, encoding and parsing: improv_proto.c (unit-tested on the host).
#define TYPE_STATE        IMPROV_TYPE_STATE
#define TYPE_ERROR        IMPROV_TYPE_ERROR
#define TYPE_RPC          IMPROV_TYPE_RPC
#define TYPE_RPC_RESULT   IMPROV_TYPE_RESULT
#define STATE_READY       IMPROV_STATE_READY
#define STATE_PROVISIONING IMPROV_STATE_PROVISIONING
#define STATE_PROVISIONED IMPROV_STATE_PROVISIONED
#define ERR_NONE          IMPROV_ERR_NONE
#define ERR_INVALID_RPC   IMPROV_ERR_INVALID_RPC
#define ERR_UNKNOWN_RPC   IMPROV_ERR_UNKNOWN_RPC
#define ERR_UNABLE_TO_CONNECT IMPROV_ERR_UNABLE_TO_CONNECT
#define CMD_WIFI_SETTINGS IMPROV_CMD_WIFI_SETTINGS
#define CMD_GET_STATE     IMPROV_CMD_GET_STATE
#define CMD_GET_INFO      IMPROV_CMD_GET_INFO
#define CMD_GET_NETWORKS  IMPROV_CMD_GET_NETWORKS
#define CMD_VENDOR_STATUS IMPROV_CMD_VENDOR_STATUS

static void port_write(port_t port, const uint8_t *data, size_t len)
{
    if (port == PORT_USB_JTAG) {
        usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
    } else {
        uart_write_bytes(UART_NUM_0, data, len);
    }
}

static int port_read(port_t port, uint8_t *buf, size_t len)
{
    if (port == PORT_USB_JTAG) {
        return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(100));
    }
    return uart_read_bytes(UART_NUM_0, buf, len, pdMS_TO_TICKS(100));
}

static void send_packet(port_t port, uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t pkt[IMPROV_MAX_PACKET];
    port_write(port, pkt, improv_encode(type, data, len, pkt));
}

static void send_state(port_t port, uint8_t state) { send_packet(port, TYPE_STATE, &state, 1); }
static void send_error(port_t port, uint8_t err)   { send_packet(port, TYPE_ERROR, &err, 1); }

/** RPC result: [cmd][total len][len][str]... */
/** RPC result: [cmd][total len][len][str]... */
static void send_result(port_t port, uint8_t cmd, const char *const *strs, int count)
{
    uint8_t buf[255];
    size_t n = improv_result_payload(cmd, strs, count, buf);
    send_packet(port, TYPE_RPC_RESULT, buf, (uint8_t)n);
}

static void send_url(port_t port, uint8_t cmd)
{
    char url[64];
    if (net_device_url(url, sizeof(url)) == ESP_OK) {
        const char *s[] = { url };
        send_result(port, cmd, s, 1);
    } else {
        send_result(port, cmd, NULL, 0);
    }
}

static void handle_rpc(port_t port, const uint8_t *d, uint8_t len)
{
    uint8_t cmd, args_len;
    const uint8_t *args;
    if (!improv_rpc_split(d, len, &cmd, &args, &args_len)) {
        send_error(port, ERR_INVALID_RPC);
        return;
    }
    switch (cmd) {
    case CMD_GET_STATE:
        if (net_is_connected()) {
            send_state(port, STATE_PROVISIONED);
            send_url(port, cmd);
        } else {
            send_state(port, STATE_READY);
        }
        break;

    case CMD_GET_INFO: {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *s[] = { "ESP32 Voice Assistant", app->version, "ESP32-S3", "assistant" };
        send_result(port, cmd, s, 4);
        break;
    }

    case CMD_WIFI_SETTINGS: {
        char ssid[33], pass[65];
        if (!improv_parse_wifi(args, args_len, ssid, pass)) {
            send_error(port, ERR_INVALID_RPC);
            return;
        }

        send_error(port, ERR_NONE);
        send_state(port, STATE_PROVISIONING);
        if (net_provision(ssid, pass, 15000) == ESP_OK) {
            ESP_LOGI(TAG, "provisioned '%s'", ssid);
            send_state(port, STATE_PROVISIONED);
            send_url(port, cmd);
        } else {
            send_error(port, ERR_UNABLE_TO_CONNECT);
            send_state(port, STATE_READY);
        }
        break;
    }

    case CMD_GET_NETWORKS: {
        // One result per network: [ssid, rssi, "YES"/"NO" secured], then an empty result.
        wifi_ap_record_t *aps = calloc(20, sizeof(*aps));
        int n = aps ? net_scan(aps, 20) : 0;
        for (int i = 0; i < n; i++) {
            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
            const char *s[] = { (const char *)aps[i].ssid, rssi,
                                aps[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES" };
            send_result(port, cmd, s, 3);
        }
        free(aps);
        send_result(port, cmd, NULL, 0);
        break;
    }

    case CMD_VENDOR_STATUS: {
        char ssid[33], saved[33], url[64] = "", rssi[8] = "";
        net_current_ssid(ssid, sizeof(ssid));
        net_saved_ssid(saved, sizeof(saved));
        net_device_url(url, sizeof(url));
        if (ssid[0]) snprintf(rssi, sizeof(rssi), "%d", net_rssi());
        const char *s[] = { ssid, url, rssi, saved };
        send_result(port, cmd, s, 4);
        break;
    }

    default:
        send_error(port, ERR_UNKNOWN_RPC);
        break;
    }
}

static void improv_task(void *arg)
{
    const port_t port = (port_t)(intptr_t)arg;
    improv_parser_t parser;
    improv_parser_reset(&parser);
    improv_packet_t pkt;
    uint8_t c;
    for (;;) {
        if (port_read(port, &c, 1) != 1) continue;
        int r = improv_parser_feed(&parser, c, &pkt);
        if (r > 0 && pkt.version == IMPROV_VERSION && pkt.type == TYPE_RPC) {
            handle_rpc(port, pkt.data, pkt.len);
        } else if (r < 0) {
            send_error(port, ERR_INVALID_RPC);
        }
    }
}

esp_err_t improv_start(void)
{
    // UART0 (USB-UART bridge boards). The console keeps logging through it.
    esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        xTaskCreate(improv_task, "improv_uart", 4096, (void *)PORT_UART0, 3, NULL);
    } else {
        ESP_LOGW(TAG, "UART0 unavailable: %s", esp_err_to_name(err));
    }

    // Built-in USB Serial/JTAG (native USB boards). Harmless if not wired.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        xTaskCreate(improv_task, "improv_usb", 4096, (void *)PORT_USB_JTAG, 3, NULL);
    } else {
        ESP_LOGW(TAG, "USB Serial/JTAG unavailable: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
