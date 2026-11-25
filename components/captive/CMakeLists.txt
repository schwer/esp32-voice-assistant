idf_component_register(
    SRCS "captive.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server json net setup_web lwip esp_wifi
)
