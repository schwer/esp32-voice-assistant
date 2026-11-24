// SPDX-License-Identifier: MIT
// LittleFS on the "storage" partition, mounted at /data, plus an optional microSD
// card at /sdcard. New audio recordings (messages in Gemini's voice, intercom
// messages) go to the card when there is one, under /sdcard/jarvis.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_ROOT    "/data"
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_SD_DIR  STORAGE_SD_ROOT "/jarvis"

esp_err_t storage_init(void);
/** Total and used bytes of the storage partition. */
esp_err_t storage_info(size_t *total, size_t *used);

/** Mount the microSD card if one is inserted (never formats it). Call once at start. */
esp_err_t storage_sd_init(void);
/** A card is mounted and writable. */
bool storage_sd_ready(void);
/** Card size and free space in bytes (ESP_ERR_INVALID_STATE without a card). */
esp_err_t storage_sd_info(uint64_t *total, uint64_t *free_bytes);
/** Plain-language card status for diagnostics ("32 GB card, 31.2 GB free", "no card"...). */
const char *storage_sd_status(void);

/** Where new recordings go: STORAGE_SD_DIR with a card, else STORAGE_ROOT. */
const char *storage_recordings_root(void);
