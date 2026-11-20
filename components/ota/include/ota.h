// SPDX-License-Identifier: MIT
//
// Updating the firmware over Wi-Fi, only when the user asks ("Jarvis, update yourself"
// or the diagnostics page). Safety:
//   - only from the HTTPS site compiled in (CONFIG_VA_UPDATE_URL), certificate-checked,
//     and only files next to its manifest;
//   - the download must match the manifest's size and SHA-256, and be this project's
//     firmware for this chip; older versions are never installed;
//   - the new version must start and reach Wi-Fi, or the board goes back to the old one.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    bool checked;           // a check has completed (ok or not)
    bool ok;                // the manifest was read
    bool available;         // a newer version can be installed over Wi-Fi
    bool needs_usb;         // newer version exists but needs one install from the web page
    char current[48];
    char latest[48];
    char why[120];          // plain-language reason when !ok or needs_usb
} ota_status_t;

/** Call early at boot (handles a version that hasn't proven itself yet). */
void ota_init(void);
/** Fetch the manifest and compare (needs Wi-Fi; a few seconds). */
esp_err_t ota_check(ota_status_t *out);
/** The last check's result (zeroed if none yet). */
ota_status_t ota_last_status(void);
/** Check once now and then every 12 hours while online (never installs anything). */
void ota_start_background_checks(bool (*online)(void));

/** Ask for an install (voice or diagnostics page). The app runs it when it's idle. */
void ota_request_install(void);
/** True once per request: the app should run ota_install() now. */
bool ota_take_install_request(void);
/** Download, verify and select the new version. On ESP_OK, restart to use it. */
esp_err_t ota_install(void (*progress)(int percent));
const char *ota_last_error(void);

/** This boot is a new version on trial (not yet confirmed good). */
bool ota_on_trial(void);
/** Keep this version. Returns the version updated from ("" if this wasn't an update). */
const char *ota_confirm(void);
/** Give up on this version and restart into the previous one. */
void ota_roll_back(void);
/** True once after an update that was rolled back (to say so). */
bool ota_take_rolled_back(void);

/** True once per new version found by the background check: tell the user about it. */
bool ota_should_mention(char *version, size_t len);

/** Gemini tool: check_for_update. */
cJSON *ota_tool_check(void);
