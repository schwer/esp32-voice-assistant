idf_component_register(
    SRCS "ota.c" "ota_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_common
    PRIV_REQUIRES app_update esp_http_client esp-tls esp_partition esp_timer mbedtls nvs_flash bootloader_support esp_app_format heap
)
