// SPDX-License-Identifier: MIT
// Diagnostics: recent log capture, module status, and a JSON health snapshot.
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

/** Start capturing log output into a RAM ring buffer (logs still go to serial). */
esp_err_t diag_init(void);

/** Record a module's status, e.g. diag_set_status("audio", "not implemented"). */
void diag_set_status(const char *module, const char *status);

/** Health snapshot (caller frees). Never contains secrets. */
cJSON *diag_snapshot(void);

/** Copy recent log text into buf (NUL-terminated). Returns bytes copied. */
size_t diag_logs(char *buf, size_t len);

/** Check the saved API key against Google: {"ok":bool,"http_status":N,"message":"..."} */
cJSON *diag_test_gemini(void);
