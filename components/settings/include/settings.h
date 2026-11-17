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
#define SETTINGS_MODEL_MAX   64
#define SETTINGS_VOICE_MAX   24

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

/** Gemini Live model id (no "models/" prefix) and prebuilt voice name. */
const char *settings_get_model(void);
const char *settings_get_voice(void);
esp_err_t settings_set_model(const char *model);
esp_err_t settings_set_voice(const char *voice);

/** Home location for weather ("Boston, MA"), temperature units, web search. */
#define SETTINGS_LOCATION_MAX 64
const char *settings_get_location(void);
esp_err_t settings_set_location(const char *location);
bool settings_get_fahrenheit(void);
esp_err_t settings_set_fahrenheit(bool on);
bool settings_get_search(void);
esp_err_t settings_set_search(bool on);

/** Let the user interrupt the assistant while it talks. Default on. */
bool settings_get_barge_in(void);
esp_err_t settings_set_barge_in(bool on);
/** Only answer speech meant for the assistant (Gemini "proactive audio"). Default on. */
bool settings_get_proactive(void);
esp_err_t settings_set_proactive(bool on);

/** Battery saver: Wi-Fi off after a few quiet minutes when running on battery. Default on. */
bool settings_get_battery_saver(void);
esp_err_t settings_set_battery_saver(bool on);

/** Join open Wi-Fi (no password) when none of the saved networks is around. Default on. */
bool settings_get_open_wifi(void);
esp_err_t settings_set_open_wifi(bool on);

/** Spoken status messages ("I need Wi-Fi", "Settings saved", ...). Default on. */
bool settings_get_spoken_status(void);
esp_err_t settings_set_spoken_status(bool on);
