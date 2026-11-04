idf_component_register(
    SRCS
        "src/zv_net.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        mbedtls
        espressif__cjson
        zv_audio
        freertos
        nvs_flash
        zv_board
        zv_core
        zv_storage
        zv_ui
        zv_power
)

# Lab trust anchor for CONFIG_ZV_M2_LAB_CA (created by tools/setup_m2wss.py).
# CMAKE_CURRENT_LIST_DIR is <repo>/components/zv_net, so repo root is ../...
set(_lab_ca "${CMAKE_CURRENT_LIST_DIR}/../../certs/lab_ca.pem")
if(EXISTS "${_lab_ca}")
    target_add_binary_data(${COMPONENT_LIB} "${_lab_ca}" TEXT)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
