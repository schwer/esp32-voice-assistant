idf_component_register(
    SRCS "voicepack.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES prompts gemini_live settings dsp storage
)
