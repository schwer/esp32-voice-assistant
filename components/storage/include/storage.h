// SPDX-License-Identifier: MIT
// LittleFS on the "storage" partition, mounted at /data.
#pragma once
#include "esp_err.h"

#define STORAGE_ROOT "/data"

esp_err_t storage_init(void);
