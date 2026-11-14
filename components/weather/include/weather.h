// SPDX-License-Identifier: MIT
//
// Weather from Open-Meteo (https://open-meteo.com): free, no API key, no account.
#pragma once
#include <stdbool.h>
#include "cJSON.h"

/**
 * Current conditions plus a forecast for `days` days (1-7) at `location`
 * (a place name such as "Boston" or "Paris, France").
 * Returns a JSON object for the model, or {"error": "..."} (caller frees).
 */
cJSON *weather_lookup(const char *location, int days, bool fahrenheit);
