#ifndef KV_SD_H
#define KV_SD_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// KV Store on SD Card
//
// Layout (computed at format time from SD capacity):
//   Block 0:       Superblock (magic + region offsets)
//   Blocks 1-7:    Reserved
//   Index region:  15% of SD (bitmap + sorted keys + bloom + future)
//   Data region:   remaining ~83% (4 blocks per card = 2KB)
//
// Magic: 31415926 + "Pico"
// ============================================================

#define KVSD_CARD_BLOCKS    4
#define KVSD_CARD_SIZE      (KVSD_CARD_BLOCKS * 512)
#define KVSD_MAGIC_LO       0x7D   // card data magic
#define KVSD_MAGIC_HI       0xCA

// Superblock layout (block 0)
typedef struct __attribute__((packed)) {
    uint8_t  magic[8];         // 31415926Pico
    uint32_t version;          // firmware version
    uint32_t total_cards;      // active card count

    // Index region
    uint32_t index_start;      // first block of index region
    uint32_t index_blocks;     // total blocks in index region

    // Sub-regions within index
    uint32_t bitmap_start;     // block offset of bitmap
    uint32_t bitmap_blocks;    // blocks used by bitmap
    uint32_t keylist_start;    // sorted key array start
    uint32_t keylist_blocks;   // blocks for sorted keys
    uint32_t bloom_start;      // bloom filter start
    uint32_t bloom_blocks;     // blocks for bloom filters

    // Data region
    uint32_t data_start;       // first block of data region
    uint32_t data_blocks;      // total blocks in data region
    uint32_t max_cards;        // data_blocks / KVSD_CARD_BLOCKS

    uint32_t next_free_hint;   // next likely free card slot
    uint32_t dirty;            // 1 = index needs flush

    // OTA staging region — 600KB = 1200 blocks, right after superblock
    uint32_t ota_start;        // block offset of OTA staging area
    uint32_t ota_blocks;       // 1200 (600KB)
} kvsd_superblock_t;

#define KVSD_OTA_BLOCKS  1200   // 600KB = 1200 * 512

// SD ring buffer guard — last 256 blocks reserved for UDP overflow WAL
#define SDRING_GUARD_BLOCKS  256

// SRAM index — key + slot parallel arrays (COW support)
// 18K entries × 8 bytes = 144KB (same footprint as 36K keys-only)
#define KVSD_INDEX_MAX      18000

// ============================================================
// Flash index tier (tier 2) — XIP-readable sorted (key,slot) pages
// Lives at 768KB–1MB in flash (after firmware, before KV region)
// Each 4KB sector holds 512 entries (8 bytes each)
// Total: 64 sectors × 512 = 32,768 entries
// XIP-mapped: reads are zero-copy pointer dereference
// ============================================================
#define FIDX_SECTOR_SIZE    4096
#define FIDX_ENTRY_SIZE     8        // [key:4][slot:4]
#define FIDX_PER_SECTOR     (FIDX_SECTOR_SIZE / FIDX_ENTRY_SIZE)  // 512
#define FIDX_SECTORS        64       // 256KB
#define FIDX_FLASH_OFFSET   0x0C0000  // 768KB into flash
#define FIDX_XIP_BASE       (0x10000000 + FIDX_FLASH_OFFSET)

// FIDX region header constants.
// The header occupies the first 256 bytes of the FIDX region (one flash
// programming page), and entries begin immediately after it.  Writing entries
// before the header means a power-loss during write leaves the header absent
// (or stale from the previous flush), so fidx_count_entries() returns 0 and
// the flash index is cleanly ignored until the next successful flush.
#define FIDX_MAGIC          0x46494458u  // "FIDX"
#define FIDX_VERSION        0x01u
#define FIDX_ENTRY_OFFSET   256u         // bytes: entries start after the header

// Effective entry capacity after reserving FIDX_ENTRY_OFFSET bytes for the
// header (was 32768; now 32736 — a reduction of only 32 entries).
#define FIDX_MAX_ENTRIES    ((FIDX_SECTORS * FIDX_SECTOR_SIZE - FIDX_ENTRY_OFFSET) / FIDX_ENTRY_SIZE)

// FIDX region header: written last after all entries are on flash.
// crc32 covers all entry data (entry_count * FIDX_ENTRY_SIZE bytes) so that
// any partial-write or bit-flip in the entries is detected on boot.
typedef struct __attribute__((packed)) {
    uint32_t magic;         // FIDX_MAGIC
    uint8_t  version;       // FIDX_VERSION
    uint8_t  _pad[3];
    uint32_t sequence;      // monotonically increasing per flush; used to
                            // detect stale headers from interrupted writes
    uint32_t entry_count;   // number of valid (key,slot) pairs in the region
    uint32_t crc32;         // CRC32 of (entry_count * FIDX_ENTRY_SIZE) bytes
    uint8_t  _reserved[236]; // pad header to exactly 256 bytes
} fidx_region_hdr_t;

_Static_assert(sizeof(fidx_region_hdr_t) == 256, "fidx_region_hdr_t size");

void kvsd_init(void);
bool kvsd_put(uint32_t key, const uint8_t *value, uint16_t len);
const uint8_t *kvsd_get(uint32_t key, uint16_t *len);
bool kvsd_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version);
bool kvsd_delete(uint32_t key);
bool kvsd_exists(uint32_t key);
uint32_t kvsd_range(uint32_t prefix, uint32_t mask,
                    uint32_t *out_keys, uint16_t *out_unused, uint32_t max);

typedef struct {
    uint32_t active;
    uint32_t index_max;
    uint32_t max_cards;
    uint32_t sd_mb;
} kvsd_stats_t;

kvsd_stats_t kvsd_stats(void);
uint32_t kvsd_record_count(void);
uint32_t kvsd_type_counts(uint16_t *out_types, uint32_t *out_counts, uint32_t max_types);
bool kvsd_flush(void);   // persist index + superblock to SD
bool kvsd_dirty(void);  // true if index needs flushing
bool kvsd_ready(void);

// OTA staging: returns the SD block offset for OTA firmware staging
uint32_t kvsd_ota_start_block(void);

#endif
