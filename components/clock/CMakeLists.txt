idf_component_register(
    SRCS "clock.c" "clock_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES audio prompts storage esp_timer
)
