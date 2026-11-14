// SPDX-License-Identifier: MIT
// LittleFS on the "storage" partition, mounted at /data.
#pragma once
#include <stddef.h>
#include "esp_err.h"

#define STORAGE_ROOT "/data"

esp_err_t storage_init(void);
/** Total and used bytes of the storage partition. */
esp_err_t storage_info(size_t *total, size_t *used);
