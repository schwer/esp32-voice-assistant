// SPDX-License-Identifier: MIT
#include <string.h>
#include "tools.h"
#include "appointments.h"
// TODO: declare add_appointment, list_appointments, update_appointment,
//       delete_appointment, remember_note
cJSON *tools_declarations(void) { return cJSON_CreateArray(); }
cJSON *tools_handle_call(const char *name, const cJSON *args)
{
    (void)args;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "not implemented");
    cJSON_AddStringToObject(r, "tool", name);
    return r;
}
