#include "key_store.h"
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <string.h>
#include <stdio.h>

// Store the key in the very last flash sector (4KB).
// Pico 2W has 4MB flash.
#define KEY_FLASH_OFFSET (4 * 1024 * 1024 - FLASH_SECTOR_SIZE)
#define KEY_MAGIC        0x504B4559  // "PKEY"
#define XIP_BASE_ADDR    0x10000000

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  psk[PSK_LEN];
    uint32_t checksum;  // simple sum of psk bytes
} key_record_t;

static const key_record_t *flash_key(void) {
    return (const key_record_t *)(XIP_BASE_ADDR + KEY_FLASH_OFFSET);
}

static uint32_t compute_checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

bool key_store_load(uint8_t psk[PSK_LEN]) {
    const key_record_t *rec = flash_key();

    if (rec->magic != KEY_MAGIC) return false;
    if (rec->checksum != compute_checksum(rec->psk, PSK_LEN)) return false;

    // Check it's not all 0xFF (erased flash)
    uint8_t all_ff = 0xFF;
    for (int i = 0; i < PSK_LEN; i++) all_ff &= rec->psk[i];
    if (all_ff == 0xFF) return false;

    memcpy(psk, rec->psk, PSK_LEN);
    return true;
}

void key_store_generate(uint8_t psk[PSK_LEN]) {
    // Generate random key using hardware RNG
    for (int i = 0; i < PSK_LEN; i += 4) {
        uint32_t r = get_rand_32();
        int remaining = PSK_LEN - i;
        int copy = (remaining < 4) ? remaining : 4;
        memcpy(&psk[i], &r, copy);
    }

    // Prepare flash record
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, FLASH_PAGE_SIZE);

    key_record_t *rec = (key_record_t *)page;
    rec->magic = KEY_MAGIC;
    memcpy(rec->psk, psk, PSK_LEN);
    rec->checksum = compute_checksum(psk, PSK_LEN);

    // Write to flash
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KEY_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(KEY_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    printf("[key] Generated and saved new PSK to flash\n");
}

void key_store_format_hex(const uint8_t psk[PSK_LEN], char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < PSK_LEN; i++) {
        buf[i * 2]     = hex[psk[i] >> 4];
        buf[i * 2 + 1] = hex[psk[i] & 0x0F];
    }
    buf[PSK_LEN * 2] = '\0';
}
