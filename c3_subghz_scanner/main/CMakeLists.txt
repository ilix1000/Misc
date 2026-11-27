idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES flipper_subghz esp_driver_gpio esp_timer)
