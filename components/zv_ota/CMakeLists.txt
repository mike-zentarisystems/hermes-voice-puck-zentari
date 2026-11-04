idf_component_register(
    SRCS
        "src/zv_ota.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_update
        esp-tls
        esp_common
        esp_http_client
        esp_partition
        esp_system
        freertos
        mbedtls
        zv_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)