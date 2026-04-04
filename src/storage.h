#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// PicoWAL Storage Engine
//
// Packed compressed cards on SD, indexes on flash + SRAM.
//
// SD Layout (computed at format from SD capacity):
//   Block 0:       Superblock
//   Blocks 1-7:    Reserved
//   Index region:  15% of SD (bitmap, bloom, field idx, etc.)
//   Data region:   ~83% — packed compressed cards
//
// Internal Flash Layout:
//   0x100000+: CoreLocator (key → block+offset+len)
//   0x180000+: Free-space index (block → free bytes)
//
// SRAM:
//   CoreLocator dirty buffer (inserts/deletes since last flush)
//   Pack summary cache
//   Histogram cache
//   Hot bloom filters
// ============================================================

// Card format: 0xCA7D magic + version + fields (unchanged)
#define STORE_CARD_MAGIC_LO  0x7D
#define STORE_CARD_MAGIC_HI  0xCA
#define STORE_MAX_RAW_SIZE   2048
#define STORE_BLOCK_SIZE     512

// Packed record header inside an SD block
typedef struct __attribute__((packed)) {
    uint32_t key;               // pack:10 | card:22
    uint16_t compressed_len;    // compressed data bytes (after header)
    uint16_t raw_len;           // original uncompressed length
} store_record_hdr_t;

#define STORE_RECORD_HDR_SIZE 8

// CoreLocator entry (on flash)
typedef struct __attribute__((packed)) {
    uint32_t key;
    uint32_t block;             // SD block number
    uint16_t offset;            // byte offset within block
    uint16_t compressed_len;    // compressed size
} store_locator_t;

#define STORE_LOCATOR_SIZE 12

// Free-space entry (on flash)
typedef struct __attribute__((packed)) {
    uint32_t block;
    uint16_t free_bytes;        // 0 = full, not stored. Only partially-used blocks.
    uint16_t _pad;
} store_freespace_t;

#define STORE_FREESPACE_SIZE 8

// Pack summary entry (SRAM + SD)
typedef struct {
    uint16_t pack_ord;
    uint16_t flags;             // bit 0 = public_read
    uint32_t card_count;
    uint32_t schema_key;        // key of this pack's schema card
    char     name[48];
} store_pack_summary_t;

#define STORE_MAX_PACKS 256

// Stats
typedef struct {
    uint32_t total_cards;
    uint32_t total_blocks_used;
    uint32_t max_data_blocks;
    uint32_t sd_mb;
    uint32_t locator_count;
    uint32_t freespace_count;
    uint32_t packs;
} store_stats_t;

// ---- Core API ----
bool store_init(void);              // Init SD + load indexes
bool store_format(void);            // Format SD (destructive)

// Card operations
bool store_put(uint32_t key, const uint8_t *data, uint16_t len);
bool store_get(uint32_t key, uint8_t *out, uint16_t *len);
bool store_delete(uint32_t key);
bool store_exists(uint32_t key);

// Range query (by pack prefix)
uint32_t store_range(uint32_t prefix, uint32_t mask,
                     uint32_t *out_keys, uint32_t max);

// Pack operations
const store_pack_summary_t *store_get_packs(uint32_t *count);
bool store_update_pack_summary(uint16_t pack_ord, const char *name,
                               uint16_t flags, uint32_t schema_key);

// Index maintenance
bool store_flush(void);             // Persist dirty indexes to SD/flash
store_stats_t store_stats(void);
bool store_ready(void);

// Timestamp log
bool store_log_write(uint32_t key, uint8_t op);  // op: 1=create,2=update,3=delete

#endif
