idf_component_register(
    SRCS "tools.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES appointments conversations
)
