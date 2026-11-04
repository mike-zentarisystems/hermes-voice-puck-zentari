# zv_core: portable, allocation-free application logic.
# Built as an ESP-IDF component on target and as a plain static library for
# host tests (tests/host). It must never include ESP-IDF or FreeRTOS headers.

set(ZV_CORE_SRCS
    src/zv_status.c
    src/zv_event.c
    src/zv_event_queue.c
    src/zv_app_state.c
    src/zv_backoff.c
    src/zv_circuit_breaker.c
    src/zv_link_supervisor.c
    src/zv_link_acceptance.c
    src/zv_redact.c
    src/zv_lipsync.c
    src/zv_audio_probe.c
    src/zv_avatar_model.c
    src/zv_ui_draw.c
    src/zv_volume.c
    src/zv_zenti_sprite.c
    src/zv_approval.c
    src/zv_trust_store.c
    src/zv_device_command.c
    src/zv_touch.c
    src/zv_ota_policy.c
    src/zv_memory_budget.c
    src/zv_cred_rotation.c
    src/zv_cred_store.c
    src/zv_gateway.c
)

# Both host and firmware compile the same immutable binary mascot data.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../assets/avatar/reference-v4.rgb565")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../assets/avatar/reference-v4.rgb565" ZV_REFERENCE_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," ZV_REFERENCE_BYTES "${ZV_REFERENCE_HEX}")
set(ZV_REFERENCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/zv_reference_blob.c")
file(WRITE "${ZV_REFERENCE_SOURCE}" "const unsigned char zv_reference_blob[] = {${ZV_REFERENCE_BYTES}};\nconst unsigned int zv_reference_blob_size = sizeof(zv_reference_blob);\n")
list(APPEND ZV_CORE_SRCS "${ZV_REFERENCE_SOURCE}")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ZV_CORE_SRCS}
                           INCLUDE_DIRS include)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
else()
    add_library(zv_core STATIC ${ZV_CORE_SRCS})
    target_include_directories(zv_core PUBLIC include)
    set_target_properties(zv_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(zv_core PRIVATE /W4 /WX /wd4201)
    else()
        target_compile_options(zv_core PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror)
    endif()
endif()
