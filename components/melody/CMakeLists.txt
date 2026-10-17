idf_component_register(SRCS "melody.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_gpio
                       PRIV_REQUIRES freertos esp_driver_i2s)
