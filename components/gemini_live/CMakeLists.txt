idf_component_register(
    SRCS "gemini_live.c" "gemini_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_websocket_client audio esp_timer mbedtls
)
