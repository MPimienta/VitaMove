idf_component_register(SRCS "vitamove.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos esp_timer esp_event esp_netif esp_wifi nvs_flash
                             esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi
                             esp_lcd bt
                             esp_http_client esp_https_ota app_update
                             spiffs
                             esp-tls esp_pm esp_hw_support)
