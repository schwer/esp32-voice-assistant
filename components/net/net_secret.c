// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"
#include "esp_hmac.h"
#include "mbedtls/gcm.h"
#include "net_secret.h"

static const char *TAG = "net_secret";
static uint8_t s_key[32];
static bool s_ready;

esp_err_t net_secret_init(void)
{
    if (s_ready) return ESP_OK;
    esp_efuse_block_t blk;
    if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &blk)) {
        blk = esp_efuse_find_unused_key_block();
        if (blk == EFUSE_BLK_KEY_MAX) {
            ESP_LOGW(TAG, "no free eFuse key block: Wi-Fi passwords can't be encrypted");
            return ESP_ERR_NOT_FOUND;
        }
        uint8_t secret[32];
        esp_fill_random(secret, sizeof(secret));
        // Permanent and read-protected: only the HMAC peripheral can use it from now on.
        esp_err_t err = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, secret, sizeof(secret));
        memset(secret, 0, sizeof(secret));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "couldn't store the encryption secret in eFuse: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "created the Wi-Fi password key in eFuse block %d", (int)blk);
    }
    static const char label[] = "esp32-voice-assistant wifi credentials v1";
    esp_err_t err = esp_hmac_calculate((hmac_key_id_t)(blk - EFUSE_BLK_KEY0), label, sizeof(label) - 1, s_key);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HMAC failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

bool net_secret_hardware(void) { return s_ready; }

size_t net_secret_seal(const uint8_t *in, size_t len, uint8_t *out)
{
    if (!s_ready) return 0;
    uint8_t *nonce = out + 2, *tag = out + 14, *ct = out + 30;
    out[0] = 'v';
    out[1] = '1';
    esp_fill_random(nonce, 12);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int r = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, s_key, 256);
    if (!r) r = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, NULL, 0, in, ct, 16, tag);
    mbedtls_gcm_free(&g);
    return r ? 0 : len + NET_SECRET_OVERHEAD;
}

int net_secret_open(const uint8_t *blob, size_t blob_len, uint8_t *out)
{
    if (!s_ready || blob_len < NET_SECRET_OVERHEAD || blob[0] != 'v' || blob[1] != '1') return -1;
    size_t len = blob_len - NET_SECRET_OVERHEAD;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int r = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, s_key, 256);
    if (!r) r = mbedtls_gcm_auth_decrypt(&g, len, blob + 2, 12, NULL, 0, blob + 14, 16, blob + 30, out);
    mbedtls_gcm_free(&g);
    return r ? -1 : (int)len;
}
