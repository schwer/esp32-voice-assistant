// SPDX-License-Identifier: MIT
#pragma once
#include "cJSON.h"

/** functionDeclarations array for the Gemini setup message (caller frees). */
cJSON *tools_declarations(void);
/** Dispatch a functionCall to the right handler. */
cJSON *tools_handle_call(const char *name, const cJSON *args);
