cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(pico2w_lcd C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(drivers)

add_executable(${PROJECT_NAME}
    src/main.c
    src/net_core.c
    src/wal_engine.c
    src/wal_dma.c
    src/key_store.c
    src/kv_flash.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/drivers
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_multicore
    hardware_spi
    hardware_pwm
    hardware_gpio
    hardware_dma
    hardware_flash
    pico_rand
    lcd_driver
    touch_driver
)

pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

pico_add_extra_outputs(${PROJECT_NAME})
