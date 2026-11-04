idf_component_register(
    SRCS
        "src/zv_provision_demo.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        freertos
        zv_board
        zv_core
        zv_storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
