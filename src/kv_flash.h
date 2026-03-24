#ifndef KV_FLASH_H
#define KV_FLASH_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Flash KV Store — 1 key = 1 sector (4KB)
//
// Layout:
//   [0x000000 - 0x0FFFFF]  Firmware (1MB)
//   [0x100000 - 0x3FEFFF]  KV store (3,068KB = 767 sectors)
//   [0x3FF000 - 0x3FFFFF]  PSK key store (last sector)
//
// Each 4KB sector:
//   [kv_header_t: 16 bytes][value: up to 4080 bytes]
//
// Sector states:
//   magic = 0xFFFFFFFF → free (erased)
//   magic = KV_MAGIC   → active record
//   magic = 0x00000000 → dead (invalidated, awaiting erase)
//
// Write = erase sector + program header + value
// Read  = scan for key, read from XIP memory-mapped flash
// Delete = invalidate magic (1→0 bit flip, no erase needed)
//
// SRAM ring buffers handle the I/O pipeline between cores.
// Flash is the source of truth.
// ============================================================

#define KV_FLASH_TOTAL     (4 * 1024 * 1024)
#define KV_SECTOR_SIZE     4096
#define KV_PAGE_SIZE       256

#define KV_REGION_START    (1024 * 1024)   // after 1MB firmware
#define KV_REGION_END      (KV_FLASH_TOTAL - KV_SECTOR_SIZE)  // before PSK
#define KV_SECTOR_COUNT    ((KV_REGION_END - KV_REGION_START) / KV_SECTOR_SIZE)  // 767

#define KV_MAGIC           0x4B565331  // "KVS1"
#define KV_FREE            0xFFFFFFFF
#define KV_DEAD            0x00000000

#define KV_MAX_VALUE       (KV_SECTOR_SIZE - 16)  // 4080 bytes

typedef struct __attribute__((packed)) {
    uint32_t magic;       // KV_MAGIC, KV_FREE, or KV_DEAD
    uint32_t key;         // [RecordTypeId:10][RecordId:22]
    uint16_t value_len;   // 0–4080
    uint16_t version;     // monotonic per key, for conflict detection
    uint32_t checksum;    // CRC32 of value bytes
} kv_header_t;

_Static_assert(sizeof(kv_header_t) == 16, "header must be 16 bytes");

// ============================================================
// API
// ============================================================

// Init: scan flash, build in-memory key→sector index.
void kv_init(void);

// Write a key-value pair. Finds old sector for key (if any),
// writes to a free sector, then invalidates the old one.
// Returns true on success.
bool kv_put(uint32_t key, const uint8_t *value, uint16_t len);

// Read a key. Returns pointer to value in XIP flash (zero-copy read!)
// and sets *len. Returns NULL if key not found.
const uint8_t *kv_get(uint32_t key, uint16_t *len);

// Delete a key. Invalidates its sector.
bool kv_delete(uint32_t key);

// Check if a key exists.
bool kv_exists(uint32_t key);

// Erase all dead sectors (reclaim space). Returns count erased.
uint32_t kv_reclaim(void);

// Stats.
typedef struct {
    uint32_t active;
    uint32_t dead;
    uint32_t free;
    uint32_t total;
} kv_stats_t;

kv_stats_t kv_stats(void);

// Range query: find all keys matching (key & prefix_mask) == (key_prefix & prefix_mask).
// Exploits sorted keymap order — binary search to start, linear scan.
// Example: all records for type 5: kv_range(EntityKey.Pack(5,0), 0xFFC00000, ...)
uint32_t kv_range(uint32_t key_prefix, uint32_t prefix_mask,
                  uint32_t *out_keys, uint16_t *out_sectors, uint32_t max_results);

#endif
