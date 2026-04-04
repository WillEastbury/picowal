cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(picowal_bootloader C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(picowal_bootloader
    main.c
)

target_link_libraries(picowal_bootloader
    pico_stdlib
    hardware_flash
)

# No USB, no UART — minimal
pico_enable_stdio_usb(picowal_bootloader 0)
pico_enable_stdio_uart(picowal_bootloader 0)

pico_add_extra_outputs(picowal_bootloader)
