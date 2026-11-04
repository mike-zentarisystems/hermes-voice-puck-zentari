idf_component_register(
    SRCS
        "src/zv_audio.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_i2c
        esp_timer
        freertos
        zv_board
        zv_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
