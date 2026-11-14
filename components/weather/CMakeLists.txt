idf_component_register(
    SRCS "weather.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_http_client mbedtls
)
