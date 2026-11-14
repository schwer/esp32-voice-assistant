// SPDX-License-Identifier: MIT
#pragma once
#include "cJSON.h"

/** "tools" array for the Gemini Live setup message (caller frees). */
cJSON *tools_declarations(void);
/** Dispatch a functionCall; returns the response object (caller frees). */
cJSON *tools_handle_call(const char *name, const cJSON *args);
/** Behaviour guidance for the tools, appended to the system instruction. */
const char *tools_system_instruction(void);
