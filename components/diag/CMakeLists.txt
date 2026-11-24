idf_component_register(
    SRCS "diag.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES settings storage conversations esp_wifi esp_netif esp_app_format
                  spi_flash esp_http_client mbedtls esp_timer audio battery net gemini_live
)
