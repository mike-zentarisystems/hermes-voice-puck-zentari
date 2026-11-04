idf_component_register(
    SRCS
        "src/zv_platform.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_update
        esp_app_format
        esp_common
        esp_partition
        zv_audio
        zv_board
        zv_core
        zv_hermes
        zv_net
        zv_ota
        zv_provision
        zv_storage
        zv_ui
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)