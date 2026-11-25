idf_component_register(
    SRCS "net.c" "net_logic.c" "net_secret.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event
    PRIV_REQUIRES nvs_flash esp_timer esp_http_client efuse esp_security mbedtls
)
