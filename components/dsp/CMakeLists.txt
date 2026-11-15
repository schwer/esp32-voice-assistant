# Pure audio maths, no ESP-IDF dependencies: unit-tested on the host (tests/host).
idf_component_register(
    SRCS "dsp.c"
    INCLUDE_DIRS "include"
)
