idf_component_register(
    SRCS "gemini_live.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_websocket_client audio mbedtls esp_timer
)
