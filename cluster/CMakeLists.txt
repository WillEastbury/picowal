cmake_minimum_required(VERSION 3.13)

# ============================================================
# PicoCluster — Unified firmware for RP2350 compute cluster
# Single binary, runtime hardware detection determines role
# ============================================================

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(picocluster C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ============================================================
# Unified firmware target
# ============================================================

add_executable(picocluster
    main.c
    # Common
    common/vm.c
    common/crc16.c
    common/card_cache.c
    common/ring.c
    # Detection
    detect/detect.c
    # Drivers
    drivers/w5500.c
    drivers/sd.c
    # Roles
    roles/worker.c
    roles/head.c
    roles/storage.c
)

target_include_directories(picocluster PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/common
    ${CMAKE_CURRENT_LIST_DIR}/detect
    ${CMAKE_CURRENT_LIST_DIR}/drivers
    ${CMAKE_CURRENT_LIST_DIR}/roles
)

target_link_libraries(picocluster
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_clocks
    hardware_spi
    hardware_sync
    hardware_watchdog
)

# PIO programs
pico_generate_pio_header(picocluster
    ${CMAKE_CURRENT_LIST_DIR}/pio/ring_uart.pio
)

# USB stdio for debug output
pico_enable_stdio_usb(picocluster 1)
pico_enable_stdio_uart(picocluster 0)

# Generate .uf2 for drag-and-drop flashing
pico_add_extra_outputs(picocluster)

# Target RP2350 (Pico 2)
set_target_properties(picocluster PROPERTIES
    PICO_PLATFORM rp2350
    PICO_BOARD pico2
)

