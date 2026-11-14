idf_component_register(
    SRCS "selftest.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio wakeword prompts leds
)
