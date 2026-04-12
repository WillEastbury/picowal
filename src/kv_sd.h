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

// SRAM index
#define KVSD_INDEX_MAX      36000

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
bool kvsd_ready(void);

// OTA staging: returns the SD block offset for OTA firmware staging
uint32_t kvsd_ota_start_block(void);

#endif
