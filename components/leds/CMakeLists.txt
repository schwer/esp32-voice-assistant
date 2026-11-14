idf_component_register(
    SRCS "leds.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board led_strip
)
