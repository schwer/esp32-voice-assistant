idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES littlefs fatfs sdmmc esp_driver_sdmmc board
)
