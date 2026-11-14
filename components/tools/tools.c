// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "tools.h"
#include "appointments.h"
#include "conversations.h"

static const char *TAG = "tools";

// ---------- declarations ----------

static cJSON *param(cJSON *props, const char *name, const char *type, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
    return p;
}

static cJSON *function(cJSON *decls, const char *name, const char *desc,
                       cJSON **props_out, const char *const *required, int nreq)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", name);
    cJSON_AddStringToObject(f, "description", desc);
    if (props_out) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "OBJECT");
        *props_out = cJSON_AddObjectToObject(params, "properties");
        if (nreq) {
            cJSON_AddItemToObject(params, "required", cJSON_CreateStringArray(required, nreq));
        }
        cJSON_AddItemToObject(f, "parameters", params);
    }
    cJSON_AddItemToArray(decls, f);
    return f;
}

cJSON *tools_declarations(void)
{
    cJSON *decls = cJSON_CreateArray();
    cJSON *props;

    static const char *const save_req[] = { "title", "summary" };
    function(decls, "save_conversation",
             "Save the current conversation so the user can resume it later. Call when the user "
             "asks to remember, save or keep this conversation.",
             &props, save_req, 2);
    param(props, "title", "STRING",
          "Short, distinctive spoken title, 2-6 words, e.g. 'Italy trip planning'.");
    param(props, "summary", "STRING",
          "Detailed summary to continue from later: topic, decisions, facts, names, numbers, "
          "open questions and next steps. Up to about 300 words.");

    function(decls, "list_saved_conversations",
             "List saved conversations, most recent first. Call when the user wants to resume, "
             "continue or go back to an earlier conversation.",
             NULL, NULL, 0);

    static const char *const id_req[] = { "id" };
    function(decls, "load_conversation",
             "Load a saved conversation by id to continue it. Returns its summary and recent "
             "transcript.",
             &props, id_req, 1);
    param(props, "id", "INTEGER", "The id from list_saved_conversations.");

    function(decls, "delete_conversation",
             "Delete a saved conversation. Only after the user clearly confirms.",
             &props, id_req, 1);
    param(props, "id", "INTEGER", "The id from list_saved_conversations.");

    function(decls, "end_conversation",
             "End the conversation. Call after saying a short goodbye, when the user says "
             "goodbye or that they're done.",
             NULL, NULL, 0);

    // TODO: appointment tools (add/list/update/delete_appointment, remember_note)

    cJSON *tools = cJSON_CreateArray();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddItemToObject(t, "functionDeclarations", decls);
    cJSON_AddItemToArray(tools, t);
    return tools;
}

const char *tools_system_instruction(void)
{
    return
        "Saved conversations:\n"
        "- When the user asks you to remember or save this conversation, call save_conversation "
        "with a short title and a thorough summary, then confirm briefly using the title.\n"
        "- When the user wants to resume an earlier conversation, call list_saved_conversations. "
        "If there are none, say so. If there is exactly one, offer it. Otherwise say \"Which one?\" "
        "and read the titles one at a time, most recent first, with a natural relative date "
        "(for example \"Italy trip planning, from yesterday\"), pausing briefly after each.\n"
        "- If the user interrupts with \"that one\" or similar, choose the title you were reading "
        "when they interrupted. If they name a title or a number, use that. If it is unclear, "
        "ask a short yes/no question to confirm.\n"
        "- After load_conversation, say in one sentence where you left off, then continue the "
        "conversation as if it never stopped. Saving again updates the same saved conversation.\n"
        "- Never read ids aloud.\n";
}

// ---------- dispatch ----------

static int arg_int(const cJSON *args, const char *name)
{
    const cJSON *v = cJSON_GetObjectItem(args, name);
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v)) return atoi(v->valuestring);
    return 0;
}

static const char *arg_str(const cJSON *args, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(args, name));
}

cJSON *tools_handle_call(const char *name, const cJSON *args)
{
    ESP_LOGI(TAG, "tool call: %s", name);

    if (strcmp(name, "save_conversation") == 0) {
        return conversations_save(arg_str(args, "title"), arg_str(args, "summary"));
    }
    if (strcmp(name, "list_saved_conversations") == 0) {
        return conversations_list();
    }
    if (strcmp(name, "load_conversation") == 0) {
        return conversations_load(arg_int(args, "id"));
    }
    if (strcmp(name, "delete_conversation") == 0) {
        return conversations_delete(arg_int(args, "id"));
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "unknown tool");
    return r;
}
