idf_component_register(SRCS "main.c" "app.c" "display.c"
                       INCLUDE_DIRS "."
                       REQUIRES flipper_subghz u8g2 esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_timer)
