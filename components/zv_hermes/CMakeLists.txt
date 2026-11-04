idf_component_register(
    SRCS
        "src/zv_hermes.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        mbedtls
        zv_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)