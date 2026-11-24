// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "board.h"
#include "storage.h"

static const char *TAG = "storage";
static sdmmc_card_t *s_card;
static bool s_sd_ok;
static char s_sd_status[96] = "not checked";

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

static void human(uint64_t bytes, char *buf, size_t len)
{
    double gb = bytes / 1e9;
    if (gb >= 1) snprintf(buf, len, "%.1f GB", gb);
    else snprintf(buf, len, "%.0f MB", bytes / 1e6);
}

esp_err_t storage_sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,  // never wipe the user's card
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;          // 20 MHz: plenty for audio, forgiving wiring
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;                                  // the board wires one data line
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_SD_ROOT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        if (err == ESP_FAIL) {
            strlcpy(s_sd_status, "card found but not readable: format it as FAT32 (exFAT isn't supported)",
                    sizeof(s_sd_status));
        } else {
            strlcpy(s_sd_status, "no card", sizeof(s_sd_status));
        }
        ESP_LOGI(TAG, "microSD: %s (%s)", s_sd_status, esp_err_to_name(err));
        return err;
    }
    // Make sure we can actually write there (a locked or full card counts as no card).
    mkdir(STORAGE_SD_DIR, 0755);
    FILE *f = fopen(STORAGE_SD_DIR "/.probe", "w");
    bool ok = f && fputs("ok\n", f) >= 0;
    if (f) ok &= fclose(f) == 0;
    remove(STORAGE_SD_DIR "/.probe");
    if (!ok) {
        strlcpy(s_sd_status, "card is read-only or full: recordings stay in the board's memory", sizeof(s_sd_status));
        ESP_LOGW(TAG, "microSD: %s", s_sd_status);
        return ESP_FAIL;
    }
    s_sd_ok = true;
    uint64_t total = 0, free_b = 0;
    storage_sd_info(&total, &free_b);
    char t[16], fr[16];
    human(total, t, sizeof(t));
    human(free_b, fr, sizeof(fr));
    snprintf(s_sd_status, sizeof(s_sd_status), "%s card, %s free: new recordings go here", t, fr);
    ESP_LOGI(TAG, "microSD mounted at %s: %s", STORAGE_SD_ROOT, s_sd_status);
    return ESP_OK;
}

bool storage_sd_ready(void) { return s_sd_ok; }

esp_err_t storage_sd_info(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(STORAGE_SD_ROOT, total, free_bytes);
}

const char *storage_sd_status(void) { return s_sd_status; }

const char *storage_recordings_root(void)
{
    return s_sd_ok ? STORAGE_SD_DIR : STORAGE_ROOT;
}
