#ifndef FIELD_INDEX_H
#define FIELD_INDEX_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Field Value Index — O(1) hash-based search
//
// Hash table on SD: bucket_id = fnv1a(pack, field, value) % N
// Each bucket = 1 SD block (512 bytes) = 42 entries
// 
// Supports: bool, integers, text exact match, text prefix
// ============================================================

#define FIDX_BUCKET_COUNT   65536   // 64K buckets
#define FIDX_ENTRY_SIZE     12
#define FIDX_ENTRIES_PER_BLK ((512 - 8) / FIDX_ENTRY_SIZE)  // 42

// Entry in a bucket
typedef struct __attribute__((packed)) {
    uint16_t pack;
    uint8_t  field_ord;
    uint8_t  value_type;    // 0=bool, 1=int, 2=text
    uint32_t value_hash;    // hash of the actual value
    uint32_t card_id;       // card ordinal (bottom 22 bits of key)
} fidx_entry_t;

// Bucket header
typedef struct __attribute__((packed)) {
    uint16_t count;         // entries in this bucket
    uint32_t overflow;      // next bucket block (0 = none)
    uint16_t _pad;
} fidx_bucket_hdr_t;

// Initialize — needs the SD block offset of the field index region
void fidx_init(uint32_t sd_start_block, uint32_t sd_block_count);

// Index a field value for a card
bool fidx_insert(uint16_t pack, uint32_t card_id, 
                 uint8_t field_ord, uint8_t value_type,
                 const uint8_t *value, uint16_t value_len);

// Remove all entries for a card (on delete)
bool fidx_remove_card(uint16_t pack, uint32_t card_id);

// Search: find cards where field = value. Returns count.
uint32_t fidx_search(uint16_t pack, uint8_t field_ord,
                     const uint8_t *value, uint16_t value_len,
                     uint32_t *out_card_ids, uint32_t max_results);

// Search by prefix (text fields)
uint32_t fidx_search_prefix(uint16_t pack, uint8_t field_ord,
                            const uint8_t *prefix, uint16_t prefix_len,
                            uint32_t *out_card_ids, uint32_t max_results);

// FNV-1a hash
uint32_t fidx_hash(const uint8_t *data, uint32_t len);

#endif
