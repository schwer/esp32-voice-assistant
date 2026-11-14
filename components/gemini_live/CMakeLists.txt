idf_component_register(
    SRCS "gemini_live.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_websocket_client audio
)
