idf_component_register(
    SRCS
        "src/zv_storage.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        esp_partition
        nvs_flash
        zv_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
