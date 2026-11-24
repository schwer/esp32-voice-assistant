idf_component_register(
    SRCS "intercom.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES audio dsp gemini_live settings storage leds voicepack
)
