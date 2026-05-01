#ifndef PICOCLUSTER_CARD_CACHE_H
#define PICOCLUSTER_CARD_CACHE_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Card cache — SRAM hot cache backed by flash cold store
// ============================================================

#define CARD_CACHE_SRAM_SIZE   (128 * 1024)   // 128KB SRAM cache
#define CARD_CACHE_MAX_ENTRIES 256             // Max cached cards
#define CARD_MAX_SIZE          4096            // Max card bytecode bytes

// Flash layout for cold storage
#define FLASH_CARD_BASE        0x00040000      // After 256KB firmware
#define FLASH_CARD_INDEX       0x00040000      // 2KB index
#define FLASH_CARD_DATA        0x00040800      // Bytecode storage starts here
#define FLASH_CARD_END         0x00400000      // End of 4MB flash

// Card index entry (stored in SRAM for fast lookup)
typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint16_t version;
    uint8_t *sram_ptr;        // Pointer into SRAM cache (NULL = not cached)
    uint32_t flash_offset;    // Offset in flash (0 = not persisted)
    uint32_t bytecode_len;    // Length in bytes
    uint32_t last_used;       // Tick counter for LRU
    uint16_t use_count;       // Execution count
    uint16_t flags;
} card_entry_t;

#define CARD_FLAG_DIRTY    (1 << 0)  // In SRAM, not yet in flash
#define CARD_FLAG_PINNED   (1 << 1)  // Don't evict from SRAM

// Card cache state
typedef struct {
    card_entry_t entries[CARD_CACHE_MAX_ENTRIES];
    uint16_t     entry_count;
    uint32_t     tick;            // Monotonic counter for LRU
    // SRAM pool
    uint8_t      sram_pool[CARD_CACHE_SRAM_SIZE];
    uint32_t     sram_used;
} card_cache_t;

// Initialise the card cache (load index from flash if present)
void card_cache_init(card_cache_t *cache);

// Look up a card by major/minor — returns pointer to bytecode or NULL
uint32_t *card_cache_get(card_cache_t *cache, uint8_t major, uint8_t minor,
                         uint32_t *out_len_words);

// Store a card in the cache (SRAM). Evicts LRU if full.
bool card_cache_store(card_cache_t *cache, uint8_t major, uint8_t minor,
                      uint16_t version, const uint8_t *bytecode, uint32_t len);

// Check if we have a card (without touching LRU)
bool card_cache_has(card_cache_t *cache, uint8_t major, uint8_t minor);

// Flush dirty cards to flash (call during idle)
void card_cache_flush_to_flash(card_cache_t *cache);

// Load cards from flash into SRAM on boot
void card_cache_warm_from_flash(card_cache_t *cache);

// Evict least-recently-used entry to make room
bool card_cache_evict_lru(card_cache_t *cache, uint32_t needed_bytes);

// Get cache stats
typedef struct {
    uint16_t total_cards;
    uint16_t sram_resident;
    uint32_t sram_bytes_used;
    uint32_t sram_bytes_free;
    uint32_t hits;
    uint32_t misses;
} card_cache_stats_t;

void card_cache_get_stats(card_cache_t *cache, card_cache_stats_t *stats);

#endif // PICOCLUSTER_CARD_CACHE_H
