// SPDX-License-Identifier: MIT
// User settings stored in NVS (namespace "va"), written by the setup page.
// Never compiled into the firmware image, except menuconfig fallbacks for dev builds.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define SETTINGS_API_KEY_MAX 128
#define SETTINGS_TZ_MAX      64
#define SETTINGS_NAME_MAX    32

esp_err_t settings_init(void);

const char *settings_get_tz(void);         // POSIX TZ string
const char *settings_get_name(void);       // assistant's name
bool settings_has_api_key(void);
/** Copies the API key into buf. Never expose it over the network. */
esp_err_t settings_get_api_key(char *buf, size_t len);
/** Last 4 characters of the key for display, e.g. "x7Qa". Empty if unset. */
void settings_api_key_hint(char *buf, size_t len);

esp_err_t settings_set_tz(const char *tz);
esp_err_t settings_set_name(const char *name);
esp_err_t settings_set_api_key(const char *key);
