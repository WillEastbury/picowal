#include "kv_flash.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

#define XIP_BASE_ADDR 0x10000000

// ============================================================
// Sorted keymap: sparse array of (key, sector) pairs
// Binary search for O(log n) lookup, shift-insert to maintain order.
// Max entries = KV_SECTOR_COUNT (767).
// ============================================================

typedef struct {
    uint32_t key;
    uint16_t sector;
} keymap_entry_t;

static keymap_entry_t g_keymap[KV_SECTOR_COUNT];
static uint32_t g_keymap_count = 0;
static uint32_t g_next_free_scan = 0;

// Binary search: returns index of key, or insertion point (negative - 1)
static int keymap_search(uint32_t key) {
    int lo = 0, hi = (int)g_keymap_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_keymap[mid].key == key) return mid;
        if (g_keymap[mid].key < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -(lo + 1);  // insertion point encoded as negative
}

static void keymap_insert(uint32_t key, uint16_t sector) {
    int idx = keymap_search(key);
    if (idx >= 0) {
        // Key exists — update sector
        g_keymap[idx].sector = sector;
        return;
    }

    // Insert at position
    int pos = -(idx + 1);
    if (g_keymap_count >= KV_SECTOR_COUNT) return;  // full

    // Shift right
    if ((uint32_t)pos < g_keymap_count) {
        memmove(&g_keymap[pos + 1], &g_keymap[pos],
                (g_keymap_count - pos) * sizeof(keymap_entry_t));
    }
    g_keymap[pos].key = key;
    g_keymap[pos].sector = sector;
    g_keymap_count++;
}

static void keymap_remove(uint32_t key) {
    int idx = keymap_search(key);
    if (idx < 0) return;

    // Shift left
    if ((uint32_t)idx < g_keymap_count - 1) {
        memmove(&g_keymap[idx], &g_keymap[idx + 1],
                (g_keymap_count - idx - 1) * sizeof(keymap_entry_t));
    }
    g_keymap_count--;
}

static int16_t keymap_get(uint32_t key) {
    int idx = keymap_search(key);
    if (idx >= 0) return (int16_t)g_keymap[idx].sector;
    return -1;
}

static void keymap_clear(void) {
    g_keymap_count = 0;
}

// ---- Flash helpers ----

static const kv_header_t *sector_hdr(uint32_t idx) {
    return (const kv_header_t *)(XIP_BASE_ADDR + KV_REGION_START + idx * KV_SECTOR_SIZE);
}

static const uint8_t *sector_value(uint32_t idx) {
    return (const uint8_t *)sector_hdr(idx) + sizeof(kv_header_t);
}

static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

static int find_free_sector(void) {
    for (uint32_t i = 0; i < KV_SECTOR_COUNT; i++) {
        uint32_t idx = (g_next_free_scan + i) % KV_SECTOR_COUNT;
        if (sector_hdr(idx)->magic == KV_FREE) {
            g_next_free_scan = (idx + 1) % KV_SECTOR_COUNT;
            return (int)idx;
        }
    }
    return -1;
}

// ============================================================
// Init: scan flash, build sorted keymap
// ============================================================

void kv_init(void) {
    keymap_clear();

    uint32_t active = 0, dead = 0, free_count = 0;

    // First pass: collect all valid entries, keep highest version per key
    for (uint32_t i = 0; i < KV_SECTOR_COUNT; i++) {
        const kv_header_t *hdr = sector_hdr(i);

        if (hdr->magic == KV_MAGIC) {
            if (hdr->value_len <= KV_MAX_VALUE &&
                crc32(sector_value(i), hdr->value_len) == hdr->checksum) {

                int16_t existing = keymap_get(hdr->key);
                if (existing >= 0) {
                    const kv_header_t *old = sector_hdr((uint32_t)existing);
                    if (hdr->version > old->version) {
                        keymap_insert(hdr->key, (uint16_t)i);
                    }
                } else {
                    keymap_insert(hdr->key, (uint16_t)i);
                }
                active++;
            } else {
                dead++;
            }
        } else if (hdr->magic == KV_DEAD) {
            dead++;
        } else {
            free_count++;
        }
    }

    printf("[kv] Init: %lu keys, %lu active sectors, %lu dead, %lu free (of %u)\n",
           (unsigned long)g_keymap_count, (unsigned long)active,
           (unsigned long)dead, (unsigned long)free_count, KV_SECTOR_COUNT);
}

// ============================================================
// Put
// ============================================================

bool kv_put(uint32_t key, const uint8_t *value, uint16_t len) {
    if (len > KV_MAX_VALUE) return false;

    int new_sector = find_free_sector();
    if (new_sector < 0) {
        kv_reclaim();
        new_sector = find_free_sector();
        if (new_sector < 0) return false;
    }

    uint16_t version = 1;
    int16_t old_sector = keymap_get(key);
    if (old_sector >= 0)
        version = sector_hdr((uint32_t)old_sector)->version + 1;

    // Build sector image
    uint8_t buf[KV_SECTOR_SIZE];
    memset(buf, 0xFF, KV_SECTOR_SIZE);

    kv_header_t *hdr = (kv_header_t *)buf;
    hdr->magic     = KV_MAGIC;
    hdr->key       = key;
    hdr->value_len = len;
    hdr->version   = version;
    hdr->checksum  = crc32(value, len);

    if (len > 0)
        memcpy(buf + sizeof(kv_header_t), value, len);

    uint32_t flash_offset = KV_REGION_START + (uint32_t)new_sector * KV_SECTOR_SIZE;
    uint32_t prog_len = (sizeof(kv_header_t) + len + KV_PAGE_SIZE - 1)
                        & ~(KV_PAGE_SIZE - 1);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_offset, KV_SECTOR_SIZE);
    flash_range_program(flash_offset, buf, prog_len);
    restore_interrupts(ints);

    // Update keymap
    keymap_insert(key, (uint16_t)new_sector);

    // Invalidate old
    if (old_sector >= 0) {
        uint32_t old_offset = KV_REGION_START + (uint32_t)old_sector * KV_SECTOR_SIZE;
        uint8_t zeros[KV_PAGE_SIZE];
        memset(zeros, 0, KV_PAGE_SIZE);
        uint32_t ints2 = save_and_disable_interrupts();
        flash_range_program(old_offset, zeros, KV_PAGE_SIZE);
        restore_interrupts(ints2);
    }

    return true;
}

// ============================================================
// Get — zero-copy from XIP flash
// ============================================================

const uint8_t *kv_get(uint32_t key, uint16_t *len) {
    int16_t sector = keymap_get(key);
    if (sector < 0) return NULL;

    const kv_header_t *hdr = sector_hdr((uint32_t)sector);
    if (hdr->magic != KV_MAGIC) {
        keymap_remove(key);
        return NULL;
    }

    if (len) *len = hdr->value_len;
    return sector_value((uint32_t)sector);
}

// ============================================================
// Delete
// ============================================================

bool kv_delete(uint32_t key) {
    int16_t sector = keymap_get(key);
    if (sector < 0) return false;

    uint32_t flash_offset = KV_REGION_START + (uint32_t)sector * KV_SECTOR_SIZE;
    uint8_t zeros[KV_PAGE_SIZE];
    memset(zeros, 0, KV_PAGE_SIZE);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(flash_offset, zeros, KV_PAGE_SIZE);
    restore_interrupts(ints);

    keymap_remove(key);
    return true;
}

bool kv_exists(uint32_t key) {
    return keymap_get(key) >= 0;
}

// ============================================================
// Range query: find all keys for a RecordTypeId
// Returns count, fills out_keys/out_sectors up to max_results.
// Exploits sorted order — binary search to start, linear scan.
// ============================================================

uint32_t kv_range(uint32_t key_prefix, uint32_t prefix_mask,
                  uint32_t *out_keys, uint16_t *out_sectors, uint32_t max_results) {
    // Find first key >= (key_prefix & prefix_mask)
    uint32_t lo_key = key_prefix & prefix_mask;
    uint32_t hi_key = lo_key | ~prefix_mask;

    // Binary search for lo_key
    int lo = 0, hi = (int)g_keymap_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_keymap[mid].key < lo_key) lo = mid + 1;
        else hi = mid - 1;
    }

    uint32_t count = 0;
    for (uint32_t i = (uint32_t)lo; i < g_keymap_count && count < max_results; i++) {
        if (g_keymap[i].key > hi_key) break;
        if ((g_keymap[i].key & prefix_mask) == (key_prefix & prefix_mask)) {
            if (out_keys) out_keys[count] = g_keymap[i].key;
            if (out_sectors) out_sectors[count] = g_keymap[i].sector;
            count++;
        }
    }
    return count;
}

// ============================================================
// Reclaim dead sectors
// ============================================================

uint32_t kv_reclaim(void) {
    uint32_t reclaimed = 0;
    for (uint32_t i = 0; i < KV_SECTOR_COUNT; i++) {
        if (sector_hdr(i)->magic == KV_DEAD) {
            uint32_t flash_offset = KV_REGION_START + i * KV_SECTOR_SIZE;
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(flash_offset, KV_SECTOR_SIZE);
            restore_interrupts(ints);
            reclaimed++;
        }
    }
    return reclaimed;
}

kv_stats_t kv_stats(void) {
    kv_stats_t s = {0, 0, 0, KV_SECTOR_COUNT};
    for (uint32_t i = 0; i < KV_SECTOR_COUNT; i++) {
        uint32_t magic = sector_hdr(i)->magic;
        if (magic == KV_MAGIC) s.active++;
        else if (magic == KV_DEAD) s.dead++;
        else s.free++;
    }
    return s;
}
