/* Firmware starts at sector 2 (after bootloader + boot config) */
FLASH(rx) : ORIGIN = 0x10002000, LENGTH = (512 * 1024 - 8 * 1024)
