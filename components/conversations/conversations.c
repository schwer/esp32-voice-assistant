// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "storage.h"
#include "conversations.h"

static const char *TAG = "convos";

#define DIR_PATH    STORAGE_ROOT "/convos"
#define INDEX_PATH  DIR_PATH "/index.json"
#define TRANSCRIPT_MAX (6 * 1024)        // tail kept per saved conversation

static SemaphoreHandle_t s_lock;
static char *s_transcript;               // rolling buffer (PSRAM)
static size_t s_len;
static int s_resumed_id;                 // 0 = new conversation

// ---------- file helpers ----------

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) == (size_t)n) {
        buf[n] = 0;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static esp_err_t write_json(const char *path, const cJSON *json)
{
    char *s = cJSON_PrintUnformatted(json);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    esp_err_t err = ESP_FAIL;
    if (f) {
        size_t n = strlen(s);
        bool ok = fwrite(s, 1, n, f) == n;
        ok &= fclose(f) == 0;
        if (ok) {
            remove(path);
            err = rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;   // atomic-ish replace
        }
    }
    free(s);
    return err;
}

static cJSON *load_index(void)
{
    char *s = read_file(INDEX_PATH);
    cJSON *idx = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!cJSON_IsArray(idx)) {
        cJSON_Delete(idx);
        idx = cJSON_CreateArray();
    }
    return idx;                          // array of {id,title,saved}, newest first
}

static void entry_path(int id, char *buf, size_t len)
{
    snprintf(buf, len, DIR_PATH "/%d.json", id);
}

static int index_find(cJSON *idx, int id)
{
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, idx) {
        if (cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id")) == id) {
            return i;
        }
        i++;
    }
    return -1;
}

static int next_id(cJSON *idx)
{
    int max = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, idx) {
        int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id"));
        if (id > max) max = id;
    }
    return max + 1;
}

static void format_time(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M (%A)", &tm);
}

static cJSON *error(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return r;
}

// ---------- public ----------

esp_err_t conversations_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_transcript = heap_caps_malloc(TRANSCRIPT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_transcript) {
        return ESP_ERR_NO_MEM;
    }
    mkdir(DIR_PATH, 0755);               // EEXIST is fine
    conversations_session_begin();
    return ESP_OK;
}

void conversations_session_begin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_len = 0;
    s_transcript[0] = 0;
    s_resumed_id = 0;
    xSemaphoreGive(s_lock);
}

void conversations_add_text(const char *role, const char *text)
{
    if (!text || !*text) {
        return;
    }
    char line[512];
    int n = snprintf(line, sizeof(line), "%s: %s\n", role, text);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_len + n > TRANSCRIPT_MAX) {
        // Drop the oldest text (keep whole lines where possible).
        size_t drop = s_len + n - TRANSCRIPT_MAX;
        char *nl = memchr(s_transcript + drop, '\n', s_len - drop);
        if (nl) drop = (nl - s_transcript) + 1;
        memmove(s_transcript, s_transcript + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_transcript + s_len, line, n);
    s_len += n;
    s_transcript[s_len] = 0;
    xSemaphoreGive(s_lock);
}

cJSON *conversations_save(const char *title, const char *summary)
{
    if (!title || !*title || !summary) {
        return error("title and summary are required");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *idx = load_index();
    time_t now = time(NULL);

    int id = s_resumed_id;
    int pos = id ? index_find(idx, id) : -1;
    if (pos >= 0) {
        cJSON_DeleteItemFromArray(idx, pos);   // re-inserted at the top below
    } else {
        id = next_id(idx);
    }

    // Entry file: keep the earlier transcript of a resumed conversation too.
    char path[48];
    entry_path(id, path, sizeof(path));
    char *transcript = NULL;
    if (pos >= 0) {
        char *old = read_file(path);
        cJSON *prev = old ? cJSON_Parse(old) : NULL;
        free(old);
        const char *pt = cJSON_GetStringValue(cJSON_GetObjectItem(prev, "transcript"));
        size_t plen = pt ? strlen(pt) : 0;
        transcript = malloc(plen + s_len + 1);
        if (transcript) {
            memcpy(transcript, pt ? pt : "", plen);
            memcpy(transcript + plen, s_transcript, s_len + 1);
        }
        cJSON_Delete(prev);
    } else {
        transcript = strdup(s_transcript);
    }
    if (!transcript) {
        cJSON_Delete(idx);
        xSemaphoreGive(s_lock);
        return error("out of memory");
    }
    size_t tlen = strlen(transcript);
    const char *tail = tlen > TRANSCRIPT_MAX ? transcript + tlen - TRANSCRIPT_MAX : transcript;

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "id", id);
    cJSON_AddStringToObject(entry, "title", title);
    cJSON_AddNumberToObject(entry, "saved", (double)now);
    cJSON_AddStringToObject(entry, "summary", summary);
    cJSON_AddStringToObject(entry, "transcript", tail);
    esp_err_t err = write_json(path, entry);
    cJSON_Delete(entry);
    free(transcript);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "id", id);
    cJSON_AddStringToObject(meta, "title", title);
    cJSON_AddNumberToObject(meta, "saved", (double)now);
    cJSON_InsertItemInArray(idx, 0, meta);

    // Enforce the limit: delete the oldest.
    while (cJSON_GetArraySize(idx) > CONVERSATIONS_MAX) {
        cJSON *last = cJSON_GetArrayItem(idx, cJSON_GetArraySize(idx) - 1);
        char old_path[48];
        entry_path((int)cJSON_GetNumberValue(cJSON_GetObjectItem(last, "id")), old_path, sizeof(old_path));
        remove(old_path);
        cJSON_DeleteItemFromArray(idx, cJSON_GetArraySize(idx) - 1);
    }
    if (err == ESP_OK) {
        err = write_json(INDEX_PATH, idx);
    }
    cJSON_Delete(idx);
    if (err == ESP_OK) {
        s_resumed_id = id;               // saving again later updates this entry
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed");
        return error("could not write to storage");
    }
    ESP_LOGI(TAG, "saved conversation %d: %s", id, title);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "saved", true);
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddStringToObject(r, "title", title);
    return r;
}

cJSON *conversations_list(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *idx = load_index();
    xSemaphoreGive(s_lock);

    cJSON *list = cJSON_CreateArray();
    int n = 1;
    cJSON *e;
    cJSON_ArrayForEach(e, idx) {
        char when[40];
        format_time((time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "saved")), when, sizeof(when));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "number", n++);
        cJSON_AddNumberToObject(o, "id", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id")));
        cJSON_AddStringToObject(o, "title", cJSON_GetStringValue(cJSON_GetObjectItem(e, "title")));
        cJSON_AddStringToObject(o, "saved", when);
        cJSON_AddItemToArray(list, o);
    }
    cJSON_Delete(idx);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "conversations", list);
    return r;
}

cJSON *conversations_load(int id)
{
    char path[48];
    entry_path(id, path, sizeof(path));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *s = read_file(path);
    cJSON *entry = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (entry) {
        s_resumed_id = id;
        cJSON *saved = cJSON_GetObjectItem(entry, "saved");
        char when[40];
        format_time((time_t)cJSON_GetNumberValue(saved), when, sizeof(when));
        cJSON_ReplaceItemInObject(entry, "saved", cJSON_CreateString(when));
    }
    xSemaphoreGive(s_lock);
    return entry ? entry : error("no saved conversation with that id");
}

cJSON *conversations_delete(int id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *idx = load_index();
    int pos = index_find(idx, id);
    esp_err_t err = ESP_OK;
    if (pos >= 0) {
        cJSON_DeleteItemFromArray(idx, pos);
        char path[48];
        entry_path(id, path, sizeof(path));
        remove(path);
        err = write_json(INDEX_PATH, idx);
        if (s_resumed_id == id) s_resumed_id = 0;
    }
    cJSON_Delete(idx);
    xSemaphoreGive(s_lock);
    if (pos < 0) return error("no saved conversation with that id");
    if (err != ESP_OK) return error("could not write to storage");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "deleted", true);
    return r;
}

int conversations_count(void)
{
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *idx = load_index();
    int n = cJSON_GetArraySize(idx);
    cJSON_Delete(idx);
    xSemaphoreGive(s_lock);
    return n;
}
