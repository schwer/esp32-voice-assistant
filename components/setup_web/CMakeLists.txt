idf_component_register(
    SRCS "setup_web.c" "web_util.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES settings net mdns esp_app_format diag leds json prompts selftest voicepack audio esp_timer clock ota
)
