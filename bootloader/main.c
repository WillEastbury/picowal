// ============================================================
// PicoWAL Bootloader — sector 0, never overwritten by OTA
//
// Layout:
//   Sector 0 (0x000000): This bootloader
//   Sector 1 (0x001000): Boot config — uint32 active slot XIP address
//   Slot A   (0x002000): Firmware image
//   Slot B   (0x082000): Firmware image
//
// On boot: read the uint32 at XIP 0x10001000.
// If it's a valid XIP address (0x10002000 or 0x10082000),
// load SP from target[0], PC from target[1], and jump.
// If invalid or 0xFFFFFFFF (erased), default to slot A.
// ============================================================

#include "pico/stdlib.h"
#include <stdint.h>

#define XIP_BASE          0x10000000
#define BOOT_CONFIG_ADDR  (XIP_BASE + 0x1000)   // sector 1
#define SLOT_A_ADDR       (XIP_BASE + 0x2000)   // sector 2
#define SLOT_B_ADDR       (XIP_BASE + 0x82000)  // sector 2 + 512KB

// Jump to a firmware image at the given XIP address.
// Reads the vector table: word 0 = initial SP, word 1 = reset handler.
static void __attribute__((naked, noreturn)) jump_to_image(uint32_t addr) {
    __asm volatile (
        "ldr r1, [r0, #0]\n"   // r1 = SP from vector table
        "ldr r2, [r0, #4]\n"   // r2 = reset handler (PC)
        "msr msp, r1\n"        // set main stack pointer
        "bx  r2\n"             // jump to reset handler
    );
}

int main(void) {
    // Read boot config from sector 1
    volatile uint32_t *config = (volatile uint32_t *)BOOT_CONFIG_ADDR;
    uint32_t target = config[0];

    // Validate: must be slot A or slot B address
    if (target != SLOT_A_ADDR && target != SLOT_B_ADDR) {
        target = SLOT_A_ADDR;  // default to slot A
    }

    // Jump to the active slot
    jump_to_image(target);

    // Never reached
    while (1) {}
}
