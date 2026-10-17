idf_component_register(SRCS "ch32v003_exio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES freertos)
