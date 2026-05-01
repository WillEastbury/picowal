// ============================================================
// PicoCluster — Unified Firmware Entry Point
// ============================================================
// Single binary for all node roles. Probes hardware at boot
// to determine role:
//   W5500 detected   → Head (network gateway + scheduler)
//   SD card detected → Storage (card server + compute)
//   Neither          → Worker (pure compute)
// ============================================================

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"

#include "common/config.h"
#include "detect/detect.h"
#include "roles/roles.h"
#include "discovery/discovery.h"

#include <stdio.h>

// Platform flash operations (used by card_cache.c)
void platform_flash_read(uint32_t offset, uint8_t *buf, uint32_t len) {
    // On RP2350, flash is memory-mapped at XIP_BASE
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + offset);
    memcpy(buf, flash_ptr, len);
}

void platform_flash_write(uint32_t offset, const uint8_t *buf, uint32_t len) {
    // Must disable interrupts and run from RAM
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, buf, len);
    restore_interrupts(ints);
}

void platform_flash_erase_sector(uint32_t offset) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

int main(void) {
    // --- Overclock ---
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // --- Init stdio (USB debug) ---
    stdio_init_all();
    sleep_ms(100);  // Brief settle time

    printf("\n[PicoCluster] Boot @ %d MHz\n", SYS_CLOCK_KHZ / 1000);

    // --- Hardware detection ---
    detect_result_t det;
    detect_probe_all(&det);

    node_role_t role;
    if (det.w5500_present) {
        role = ROLE_HEAD;
        printf("[PicoCluster] Role: HEAD (W5500 v%d)\n", det.w5500_version);
    } else if (det.sd_present) {
        role = ROLE_STORAGE;
        printf("[PicoCluster] Role: STORAGE (SD type %d)\n", det.sd_type);
    } else {
        role = ROLE_WORKER;
        printf("[PicoCluster] Role: WORKER\n");
    }

    if (det.wifi_present) {
        printf("[PicoCluster] WiFi: available\n");
    }

    // --- Enable watchdog ---
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    // --- Launch role (never returns) ---
    switch (role) {
    case ROLE_HEAD:
        role_head_run();
        break;
    case ROLE_STORAGE:
        role_storage_run();
        break;
    case ROLE_WORKER:
    default:
        role_worker_run();
        break;
    }

    // Should never reach here
    while (1) {
        watchdog_update();
        sleep_ms(1000);
    }

    return 0;
}
