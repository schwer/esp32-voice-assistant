cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# snprintf cuts long text on purpose; the size-optimised build reports more "may be
# truncated" cases. Keep them as warnings (shown in CI) rather than build failures.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format-truncation" APPEND)
project(esp32-voice-assistant)
