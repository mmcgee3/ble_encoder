idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-rotary-encoder esp_driver_gpio bt nvs_flash
)