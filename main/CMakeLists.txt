idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES usb_proto pn532 melody ch32v003_exio
    PRIV_REQUIRES esp_driver_i2c esp_hw_support
)
