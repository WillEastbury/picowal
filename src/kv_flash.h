#ifndef KV_FLASH_H
#define KV_FLASH_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Flash KV Store — paged extents in 4KB pages
//
// Layout:
//   [0x000000 - 0x0FFFFF]  Firmware (1MB)
//   [KV_REGION_START .. KV_REGION_END-1] KV data sectors
//   [KV_DEADLOG_START .. KV_DEADLOG_END-1] Dead-sector append log
//   [0x3FF000 - 0x3FFFFF]  PSK key store (last sector)
//
// Records are append-only and packed into each 4KB page.
// Record extents are single-block contiguous allocations.
// In-RAM index uses parallel sorted arrays:
//   keys[]: sorted uint32 keys
//   locs[]: packed uint32 location [page:12 | off_q:10 | len_q:10]
// where off_q/len_q are 4-byte quanta within the page.
//
// SRAM ring buffers handle the I/O pipeline between cores.
// Flash is the source of truth.
// ============================================================

#define KV_FLASH_TOTAL     (4 * 1024 * 1024)
#define KV_SECTOR_SIZE     4096
#define KV_PAGE_SIZE       256

#define KV_REGION_START    (1024 * 1024)   // after 1MB firmware
#define KV_DEADLOG_SECTORS 4
#define KV_REGION_END      (KV_FLASH_TOTAL - KV_SECTOR_SIZE - (KV_DEADLOG_SECTORS * KV_SECTOR_SIZE))
#define KV_DEADLOG_START   (KV_REGION_END)
#define KV_DEADLOG_END     (KV_FLASH_TOTAL - KV_SECTOR_SIZE)
#define KV_SECTOR_COUNT    ((KV_REGION_END - KV_REGION_START) / KV_SECTOR_SIZE)

#define KV_MAGIC           0x4B565331  // "KVS1"
#define KV_FREE            0xFFFFFFFF
#define KV_DEAD            0x00000000

#define KV_MAX_VALUE       4000
#define KV_VERSION_ANY     0xFFFF

typedef struct __attribute__((packed)) {
    uint32_t magic;       // KV_MAGIC
    uint32_t key;         // [RecordTypeId:10][RecordId:22]
    uint16_t raw_len;     // decoded payload length
    uint16_t store_len;   // encoded payload length after compression
    uint16_t version;     // monotonic per key
    uint8_t  flags;       // bit0: compressed, bit1: tombstone
    uint8_t  reserved;
    uint32_t checksum;    // CRC32 of stored bytes (encoded payload)
} kv_header_t;

_Static_assert(sizeof(kv_header_t) == 20, "header must be 20 bytes");

// ============================================================
// API
// ============================================================

// Init: scan flash, build in-memory key→sector index.
void kv_init(void);

// Write a key-value pair. Finds old sector for key (if any),
// writes to a free sector, then invalidates the old one.
// Returns true on success.
bool kv_put(uint32_t key, const uint8_t *value, uint16_t len);

// Version-checked write.
// If expected_version == KV_VERSION_ANY, no conflict check is performed.
// Otherwise write succeeds only when current version == expected_version.
// On success, *new_version is set to the published version (if non-NULL).
bool kv_put_if_version(uint32_t key, const uint8_t *value, uint16_t len,
                       uint16_t expected_version, uint16_t *new_version);

// Read a key. Returns pointer to value in XIP flash (zero-copy read!)
// and sets *len. Returns NULL if key not found.
const uint8_t *kv_get(uint32_t key, uint16_t *len);

// Read a key into caller-owned memory.
// *len is in/out: input capacity, output actual length.
// Returns false if key is missing, corrupted, or out buffer is too small.
bool kv_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version);

// Delete a key. Invalidates its sector.
bool kv_delete(uint32_t key);

// Check if a key exists.
bool kv_exists(uint32_t key);

// Erase all dead sectors (reclaim space). Returns count erased.
uint32_t kv_reclaim(void);

// Reclaim one dead sector from the append-only dead-sector log.
// Intended for background no-op compaction on Core 1.
bool kv_compact_step(void);

// Stats.
typedef struct {
    uint32_t active;
    uint32_t dead;
    uint32_t free;
    uint32_t total;
} kv_stats_t;

kv_stats_t kv_stats(void);

// Count distinct record types and records per type.
// Scans index, extracts top 10 bits of each key as type ordinal.
// Fills out_types[] and out_counts[] up to max_types. Returns count of distinct types.
uint32_t kv_type_counts(uint16_t *out_types, uint32_t *out_counts, uint32_t max_types);

// Total number of records in the index.
uint32_t kv_record_count(void);

// Range query: find all keys matching (key & prefix_mask) == (key_prefix & prefix_mask).
// Exploits sorted keymap order — binary search to start, linear scan.
// Example: all records for type 5: kv_range(EntityKey.Pack(5,0), 0xFFC00000, ...)
uint32_t kv_range(uint32_t key_prefix, uint32_t prefix_mask,
                  uint32_t *out_keys, uint16_t *out_sectors, uint32_t max_results);

#endif
