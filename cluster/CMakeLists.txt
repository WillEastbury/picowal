cmake_minimum_required(VERSION 3.13)

# PicoCluster - Distributed compute cluster firmware
# Targets: worker (Pico2), projection (Pico2W + W5500)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(picocluster C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ============================================================
# Common library (shared between worker and projection)
# ============================================================

add_library(picocluster_common INTERFACE)
target_include_directories(picocluster_common INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/common
)
target_sources(picocluster_common INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/common/vm.c
)

# ============================================================
# Worker firmware (Pico2 — pure compute node)
# ============================================================

add_executable(picocluster_worker
    worker/main.c
)

target_link_libraries(picocluster_worker
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_clocks
    hardware_sync
    picocluster_common
)

# Generate PIO header from .pio file
pico_generate_pio_header(picocluster_worker
    ${CMAKE_CURRENT_LIST_DIR}/pio/ring_uart.pio
)

# USB stdio for debug
pico_enable_stdio_usb(picocluster_worker 1)
pico_enable_stdio_uart(picocluster_worker 0)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(picocluster_worker)

# Target RP2350
set_target_properties(picocluster_worker PROPERTIES
    PICO_PLATFORM rp2350
)

# ============================================================
# Projection firmware (Pico2W — gateway + scheduler)
# ============================================================

add_executable(picocluster_projection
    projection/main.c
)

target_link_libraries(picocluster_projection
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_clocks
    hardware_spi
    picocluster_common
)

pico_generate_pio_header(picocluster_projection
    ${CMAKE_CURRENT_LIST_DIR}/pio/ring_uart.pio
)

pico_enable_stdio_usb(picocluster_projection 1)
pico_enable_stdio_uart(picocluster_projection 0)

pico_add_extra_outputs(picocluster_projection)

set_target_properties(picocluster_projection PROPERTIES
    PICO_PLATFORM rp2350
)
