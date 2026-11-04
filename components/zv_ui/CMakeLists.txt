idf_component_register(
    SRCS
        "src/zv_ui_demo.c"
        "src/zv_ui_shell.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        esp_timer
        esp_system
        freertos
        zv_board
        zv_core
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
