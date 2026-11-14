idf_component_register(
    SRCS "conversations.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES storage
)
