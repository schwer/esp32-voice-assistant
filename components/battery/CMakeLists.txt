idf_component_register(
    SRCS "battery.c" "battery_est.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_adc esp_timer nvs_flash
)
