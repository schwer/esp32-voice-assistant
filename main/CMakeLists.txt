idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
             audio wakeword gemini_live tools appointments setup_web board net improv settings leds
)
