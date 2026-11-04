idf_component_register(
    SRCS
        "src/zv_board.c"
        "src/zv_tca9554.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_adc
        esp_common
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        spi_flash
        esp_hw_support
        esp_lcd
        zv_lcd_st77916
        esp_psram
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
