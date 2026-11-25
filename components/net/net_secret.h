// SPDX-License-Identifier: MIT
//
// Encryption for saved Wi-Fi passwords: AES-256-GCM with a key derived by the chip's
// HMAC peripheral from a secret in eFuse (burned once, read-protected: software can't
// read it, so a copy of the flash doesn't reveal the passwords).
// Blob: "v1" | nonce (12) | tag (16) | ciphertext.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_SECRET_OVERHEAD (2 + 12 + 16)

/** Prepare the key (burns the eFuse secret on first use). */
esp_err_t net_secret_init(void);
/** True when the key comes from the eFuse secret (else passwords stay plain). */
bool net_secret_hardware(void);
/** Encrypt `len` bytes; out needs len + NET_SECRET_OVERHEAD. Returns blob length or 0. */
size_t net_secret_seal(const uint8_t *in, size_t len, uint8_t *out);
/** Decrypt a blob; out needs blob_len - NET_SECRET_OVERHEAD. Returns plain length, or -1. */
int net_secret_open(const uint8_t *blob, size_t blob_len, uint8_t *out);
