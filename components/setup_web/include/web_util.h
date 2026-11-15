// SPDX-License-Identifier: MIT
//
// Small web helpers for the setup and diagnostics pages: form decoding, HTML escaping
// and the cross-site request check. No ESP-IDF dependencies (unit-tested in tests/host).
#pragma once
#include <stdbool.h>

/** In-place application/x-www-form-urlencoded decoding ('+' and %XX). */
void web_url_decode(char *s);
/** Strip leading and trailing whitespace in place. */
void web_trim(char *s);
/** HTML entity for a character that must be escaped, or NULL. */
const char *web_html_entity(char c);
/**
 * Is a request with this Origin header coming from our own page (served as `host`)?
 * origin == NULL means the header was absent (non-browser clients): allowed.
 */
bool web_same_origin(const char *origin, const char *host);
