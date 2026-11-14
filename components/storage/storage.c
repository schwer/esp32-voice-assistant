// SPDX-License-Identifier: MIT
#include "esp_log.h"
#include "esp_littlefs.h"
#include "storage.h"

static const char *TAG = "storage";

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_ROOT,
        .partition_label = "storage",
        .format_if_mount_failed = true,   // first boot after flashing
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "mounted %s: %u / %u KB used", STORAGE_ROOT,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

esp_err_t storage_info(size_t *total, size_t *used)
{
    return esp_littlefs_info("storage", total, used);
}
