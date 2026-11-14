idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_adc esp_timer
)
