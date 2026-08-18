cmake_minimum_required(VERSION 3.16)

project(embedded_iap_framework C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(embedded_iap
    src/iap_bootloader.c
    src/iap_core.c
    src/iap_memory_storage.c
)

target_include_directories(embedded_iap
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(embedded_iap PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_executable(iap_core_tests
    tests/test_iap_core.c
)

target_link_libraries(iap_core_tests PRIVATE embedded_iap)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(iap_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME iap_core_tests COMMAND iap_core_tests)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME iap_tool_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tools.py
    )
endif()
