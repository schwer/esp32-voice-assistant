idf_component_register(
    SRCS "wakeword.c"
    INCLUDE_DIRS "include"
    REQUIRES audio
    PRIV_REQUIRES esp-sr
)
