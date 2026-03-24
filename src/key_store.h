#ifndef KEY_STORE_H
#define KEY_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define PSK_LEN 32

// Load PSK from flash. Returns true if a valid key exists.
bool key_store_load(uint8_t psk[PSK_LEN]);

// Generate a random PSK, save to flash, return it.
void key_store_generate(uint8_t psk[PSK_LEN]);

// Format PSK as hex string into buf (needs at least 65 bytes).
void key_store_format_hex(const uint8_t psk[PSK_LEN], char *buf);

#endif
