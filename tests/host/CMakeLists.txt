cmake_minimum_required(VERSION 3.16)
project(zv_host_tests C)

# Host tests for components/zv_core. No hardware, no ESP-IDF.
#   cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host

option(ZV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

enable_testing()

add_executable(zv_ui_preview ../../tools/ui_preview.c)
target_link_libraries(zv_ui_preview PRIVATE zv_core)
set_target_properties(zv_ui_preview PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../components/zv_core ${CMAKE_BINARY_DIR}/zv_core)

set(ZV_TEST_SUITES
    app_state
    event_queue
    backoff
    circuit_breaker
    link_supervisor
    link_acceptance
    redact
    lipsync
    avatar_model
    ui_draw
    volume
    approval
    trust_store
    device_command
    touch
    ota_policy
    ota_slot
    memory_budget
    cred_rotation
    cred_store
    gateway
    audio_probe
)

set(ZV_TEST_SRCS test_main.c)
foreach(suite IN LISTS ZV_TEST_SUITES)
    list(APPEND ZV_TEST_SRCS test_${suite}.c)
endforeach()

add_executable(zv_host_tests ${ZV_TEST_SRCS})
target_link_libraries(zv_host_tests PRIVATE zv_core)
target_include_directories(zv_host_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/fixtures)
set_target_properties(zv_host_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(zv_host_tests PRIVATE /W4)
else()
    target_compile_options(zv_host_tests PRIVATE -Wall -Wextra -Werror)
    if(ZV_SANITIZE)
        foreach(target zv_core zv_host_tests)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
endif()

foreach(suite IN LISTS ZV_TEST_SUITES)
    add_test(NAME ${suite} COMMAND zv_host_tests ${suite})
endforeach()
