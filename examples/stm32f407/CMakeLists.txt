cmake_minimum_required(VERSION 3.20)

project(embedded_iap_stm32f407 C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(STM32_CUBE_F4_ROOT "" CACHE PATH "Path to STM32Cube_FW_F4")
if(NOT STM32_CUBE_F4_ROOT)
    file(GLOB cube_roots "$ENV{USERPROFILE}/STM32Cube/Repository/STM32Cube_FW_F4_V*")
    if(cube_roots)
        list(SORT cube_roots COMPARE NATURAL ORDER DESCENDING)
        list(GET cube_roots 0 STM32_CUBE_F4_ROOT)
    endif()
endif()

if(NOT EXISTS "${STM32_CUBE_F4_ROOT}/Drivers/CMSIS/Device/ST/STM32F4xx/Include/stm32f407xx.h")
    message(FATAL_ERROR "Set STM32_CUBE_F4_ROOT to a valid STM32Cube_FW_F4 package")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(IAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMSIS_DEVICE "${STM32_CUBE_F4_ROOT}/Drivers/CMSIS/Device/ST/STM32F4xx")
set(CMSIS_CORE "${STM32_CUBE_F4_ROOT}/Drivers/CMSIS/Include")
set(HAL_ROOT "${STM32_CUBE_F4_ROOT}/Drivers/STM32F4xx_HAL_Driver")
set(STARTUP "${CMSIS_DEVICE}/Source/Templates/gcc/startup_stm32f407xx.s")

add_library(stm32_hal STATIC
    "${CMSIS_DEVICE}/Source/Templates/system_stm32f4xx.c"
    "${HAL_ROOT}/Src/stm32f4xx_hal_flash.c"
    "${HAL_ROOT}/Src/stm32f4xx_hal_flash_ex.c"
)

target_include_directories(stm32_hal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMSIS_DEVICE}/Include"
    "${CMSIS_CORE}"
    "${HAL_ROOT}/Inc"
)

target_compile_definitions(stm32_hal PUBLIC
    STM32F407xx
    USE_HAL_DRIVER
)
target_compile_options(stm32_hal PRIVATE -w)

add_library(stm32_iap_common STATIC
    "${IAP_ROOT}/src/iap_bootloader.c"
    "${IAP_ROOT}/src/iap_core.c"
    src/stm32_iap_platform.c
    src/stm32_iap_storage.c
    src/stm32_iap_test_protocol.c
)

target_include_directories(stm32_iap_common PUBLIC "${IAP_ROOT}/include")
target_link_libraries(stm32_iap_common PUBLIC stm32_hal)

target_compile_options(stm32_iap_common PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

function(add_stm32_firmware target main_source flash_origin flash_length)
    set(FLASH_ORIGIN "${flash_origin}")
    set(FLASH_LENGTH "${flash_length}")
    set(linker_script "${CMAKE_CURRENT_BINARY_DIR}/${target}.ld")
    configure_file(linker/firmware.ld.in "${linker_script}" @ONLY)

    add_executable(${target} "${STARTUP}" "${main_source}" src/syscalls.c)
    target_link_libraries(${target} PRIVATE stm32_iap_common)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_link_options(${target} PRIVATE
        "-T${linker_script}"
        --specs=nano.specs
        --specs=nosys.specs
        -Wl,--gc-sections
        -Wl,--no-warn-rwx-segments
        -Wl,--print-memory-usage
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map"
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:${target}>"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}.bin"
        COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${target}>"
        VERBATIM
    )
endfunction()

add_stm32_firmware(iap_bootloader src/bootloader_main.c 0x08000000 0x20000)
add_stm32_firmware(iap_app_a src/test_app_main.c 0x08020200 0x3FE00)
target_compile_definitions(iap_app_a PRIVATE IAP_APP_SLOT=IAP_SLOT_A)
add_stm32_firmware(iap_app_b src/test_app_main.c 0x08060200 0x3FE00)
target_compile_definitions(iap_app_b PRIVATE IAP_APP_SLOT=IAP_SLOT_B)

add_custom_command(TARGET iap_app_a POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${IAP_ROOT}/tools/pack_image.py"
            --input "${CMAKE_CURRENT_BINARY_DIR}/iap_app_a.bin"
            --output "${CMAKE_CURRENT_BINARY_DIR}/iap_app_a.iap.bin"
            --slot-address 0x08020000
            --header-size 0x200
    VERBATIM
)

add_custom_command(TARGET iap_app_b POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${IAP_ROOT}/tools/pack_image.py"
            --input "${CMAKE_CURRENT_BINARY_DIR}/iap_app_b.bin"
            --output "${CMAKE_CURRENT_BINARY_DIR}/iap_app_b.iap.bin"
            --slot-address 0x08060000
            --header-size 0x200
    VERBATIM
)
