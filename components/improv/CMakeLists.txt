idf_component_register(
    SRCS "improv.c" "improv_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag net
    PRIV_REQUIRES esp_app_format
)
