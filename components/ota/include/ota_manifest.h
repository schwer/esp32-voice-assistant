// SPDX-License-Identifier: MIT
//
// Reading the update description the installer page publishes (manifest.json, "ota"
// section, written by tools/write_manifest.py). Pure logic, tested on the host.
#pragma once
#include <stdbool.h>
#include <stddef.h>

/** Partition layout this firmware was built for. Must match LAYOUT in write_manifest.py. */
#define OTA_LAYOUT 2

typedef struct {
    char version[48];
    int layout;
    char app_path[96];
    long app_size;
    char app_sha256[65];
    char model_path[96];
    long model_size;
    char model_sha256[65];
} ota_manifest_t;

/** Parse manifest.json. False if it has no usable "ota" section. */
bool ota_parse_manifest(const char *json, ota_manifest_t *out);

/**
 * Is `latest` newer than `current`? Release versions look like "2026.11.20-abc1234".
 * An older date is never offered (no downgrades); the same date with another build
 * is. A development build (any other format) is offered any release.
 */
bool ota_is_newer(const char *current, const char *latest);

/** URL of `path` next to the manifest at `manifest_url` (query string dropped). */
bool ota_url_join(const char *manifest_url, const char *path, char *out, size_t len);
