idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES board esp_driver_i2s esp_codec_dev esp_io_expander_tca95xx_16bit
)
