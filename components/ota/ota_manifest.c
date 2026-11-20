// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "ota_manifest.h"

static void copy(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src ? src : "");
}

bool ota_parse_manifest(const char *json, ota_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON *ota = cJSON_GetObjectItem(root, "ota");
    const cJSON *app = cJSON_GetObjectItem(ota, "app");
    const cJSON *model = cJSON_GetObjectItem(ota, "model");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *app_path = cJSON_GetStringValue(cJSON_GetObjectItem(app, "path"));
    bool ok = version && version[0] && app_path && app_path[0] &&
              cJSON_IsNumber(cJSON_GetObjectItem(ota, "layout"));
    if (ok) {
        copy(out->version, sizeof(out->version), version);
        out->layout = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ota, "layout"));
        copy(out->app_path, sizeof(out->app_path), app_path);
        const cJSON *size = cJSON_GetObjectItem(app, "size");
        out->app_size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
        copy(out->app_sha256, sizeof(out->app_sha256), cJSON_GetStringValue(cJSON_GetObjectItem(app, "sha256")));
        copy(out->model_path, sizeof(out->model_path), cJSON_GetStringValue(cJSON_GetObjectItem(model, "path")));
        size = cJSON_GetObjectItem(model, "size");
        out->model_size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
        copy(out->model_sha256, sizeof(out->model_sha256), cJSON_GetStringValue(cJSON_GetObjectItem(model, "sha256")));
    }
    cJSON_Delete(root);
    return ok;
}

/** "YYYY.MM.DD-..." -> YYYYMMDD, or -1 if it isn't a release version. */
static long release_date(const char *v)
{
    int y, m, d;
    char dash;
    if (!v || sscanf(v, "%4d.%2d.%2d%c", &y, &m, &d, &dash) != 4 || dash != '-') return -1;
    if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return y * 10000L + m * 100 + d;
}

bool ota_is_newer(const char *current, const char *latest)
{
    if (!latest || !latest[0] || !current) return false;
    if (strcmp(current, latest) == 0) return false;
    long cur = release_date(current), lat = release_date(latest);
    if (lat < 0) return false;                       // never "update" to something odd
    if (cur < 0) return true;                        // development build: any release
    return lat >= cur;                               // same day, other build: newer
}

bool ota_url_join(const char *manifest_url, const char *path, char *out, size_t len)
{
    if (!manifest_url || !path || !path[0] || strstr(path, "://") || path[0] == '/' || strstr(path, "..")) {
        return false;                                // only files next to the manifest
    }
    size_t base = strcspn(manifest_url, "?#");
    while (base > 0 && manifest_url[base - 1] != '/') base--;
    if (base == 0) return false;
    int n = snprintf(out, len, "%.*s%s", (int)base, manifest_url, path);
    return n > 0 && (size_t)n < len;
}
