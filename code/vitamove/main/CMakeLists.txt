idf_component_register(SRCS "vitamove.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos driver esp_timer esp_wifi nvs_flash bt esp_lcd esp_driver_i2s esp_driver_i2c esp_driver_spi)
