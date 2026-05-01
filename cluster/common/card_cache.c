#include "card_cache.h"
#include "config.h"
#include <string.h>

// ============================================================
// Card cache implementation — SRAM pool with LRU eviction
// ============================================================

// Platform flash operations (weak — overridden per platform)
extern void platform_flash_read(uint32_t offset, uint8_t *buf, uint32_t len);
extern void platform_flash_write(uint32_t offset, const uint8_t *buf, uint32_t len);
extern void platform_flash_erase_sector(uint32_t offset);

// Internal: find entry by major/minor
static int cache_find_entry(card_cache_t *cache, uint8_t major, uint8_t minor) {
    for (uint16_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].major == major && cache->entries[i].minor == minor) {
            return (int)i;
        }
    }
    return -1;
}

// Internal: find LRU entry (lowest last_used, not pinned)
static int cache_find_lru(card_cache_t *cache) {
    int best = -1;
    uint32_t best_tick = UINT32_MAX;
    for (uint16_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].flags & CARD_FLAG_PINNED) continue;
        if (cache->entries[i].sram_ptr == NULL) continue;
        if (cache->entries[i].last_used < best_tick) {
            best_tick = cache->entries[i].last_used;
            best = (int)i;
        }
    }
    return best;
}

// Internal: allocate from SRAM pool
static uint8_t *pool_alloc(card_cache_t *cache, uint32_t size) {
    // Align to 4 bytes
    size = (size + 3) & ~3u;
    if (cache->sram_used + size > CARD_CACHE_SRAM_SIZE) {
        return NULL;
    }
    uint8_t *ptr = cache->sram_pool + cache->sram_used;
    cache->sram_used += size;
    return ptr;
}

// Internal: compact pool (simple: rebuild from entries)
static void pool_compact(card_cache_t *cache) {
    uint32_t new_used = 0;
    // Temporary — copy all live entries forward
    for (uint16_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].sram_ptr == NULL) continue;
        uint32_t len = (cache->entries[i].bytecode_len + 3) & ~3u;
        if (cache->entries[i].sram_ptr != cache->sram_pool + new_used) {
            memmove(cache->sram_pool + new_used,
                    cache->entries[i].sram_ptr, len);
            cache->entries[i].sram_ptr = cache->sram_pool + new_used;
        }
        new_used += len;
    }
    cache->sram_used = new_used;
}

void card_cache_init(card_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
}

uint32_t *card_cache_get(card_cache_t *cache, uint8_t major, uint8_t minor,
                         uint32_t *out_len_words) {
    int idx = cache_find_entry(cache, major, minor);
    if (idx < 0) return NULL;

    card_entry_t *e = &cache->entries[idx];
    if (e->sram_ptr == NULL) {
        // In flash but not in SRAM — load it
        if (e->flash_offset == 0) return NULL;

        uint32_t size = (e->bytecode_len + 3) & ~3u;
        uint8_t *ptr = pool_alloc(cache, size);
        if (!ptr) {
            // Evict LRU to make room
            if (!card_cache_evict_lru(cache, size)) return NULL;
            ptr = pool_alloc(cache, size);
            if (!ptr) return NULL;
        }
        platform_flash_read(e->flash_offset, ptr, e->bytecode_len);
        e->sram_ptr = ptr;
    }

    e->last_used = cache->tick++;
    e->use_count++;

    if (out_len_words) *out_len_words = e->bytecode_len / 4;
    return (uint32_t *)e->sram_ptr;
}

bool card_cache_store(card_cache_t *cache, uint8_t major, uint8_t minor,
                      uint16_t version, const uint8_t *bytecode, uint32_t len) {
    // Check if already exists with same or newer version
    int idx = cache_find_entry(cache, major, minor);
    if (idx >= 0) {
        if (cache->entries[idx].version >= version) {
            return true;  // Already have this or newer
        }
        // Update existing entry
        card_entry_t *e = &cache->entries[idx];
        uint32_t aligned_len = (len + 3) & ~3u;

        // If size fits in existing slot, overwrite
        if (e->sram_ptr && e->bytecode_len >= len) {
            memcpy(e->sram_ptr, bytecode, len);
            e->bytecode_len = len;
            e->version = version;
            e->flags |= CARD_FLAG_DIRTY;
            e->last_used = cache->tick++;
            return true;
        }

        // Otherwise allocate new slot (old one becomes dead space until compact)
        e->sram_ptr = NULL;  // Release old
        uint8_t *ptr = pool_alloc(cache, aligned_len);
        if (!ptr) {
            if (!card_cache_evict_lru(cache, aligned_len)) return false;
            pool_compact(cache);
            ptr = pool_alloc(cache, aligned_len);
            if (!ptr) return false;
        }
        memcpy(ptr, bytecode, len);
        e->sram_ptr = ptr;
        e->bytecode_len = len;
        e->version = version;
        e->flags |= CARD_FLAG_DIRTY;
        e->last_used = cache->tick++;
        return true;
    }

    // New entry
    if (cache->entry_count >= CARD_CACHE_MAX_ENTRIES) {
        // Evict LRU entry entirely
        int lru = cache_find_lru(cache);
        if (lru < 0) return false;
        // Swap with last and decrement count
        cache->entries[lru] = cache->entries[cache->entry_count - 1];
        cache->entry_count--;
        pool_compact(cache);
    }

    uint32_t aligned_len = (len + 3) & ~3u;
    uint8_t *ptr = pool_alloc(cache, aligned_len);
    if (!ptr) {
        if (!card_cache_evict_lru(cache, aligned_len)) return false;
        pool_compact(cache);
        ptr = pool_alloc(cache, aligned_len);
        if (!ptr) return false;
    }

    memcpy(ptr, bytecode, len);

    card_entry_t *e = &cache->entries[cache->entry_count++];
    e->major = major;
    e->minor = minor;
    e->version = version;
    e->sram_ptr = ptr;
    e->flash_offset = 0;
    e->bytecode_len = len;
    e->last_used = cache->tick++;
    e->use_count = 0;
    e->flags = CARD_FLAG_DIRTY;

    return true;
}

bool card_cache_has(card_cache_t *cache, uint8_t major, uint8_t minor) {
    return cache_find_entry(cache, major, minor) >= 0;
}

bool card_cache_evict_lru(card_cache_t *cache, uint32_t needed_bytes) {
    (void)needed_bytes;
    int lru = cache_find_lru(cache);
    if (lru < 0) return false;

    card_entry_t *e = &cache->entries[lru];

    // Flush to flash if dirty before evicting
    if ((e->flags & CARD_FLAG_DIRTY) && e->sram_ptr) {
        // Find a free flash slot
        // Simple: append after last written offset
        // (Real implementation needs proper flash management)
    }

    e->sram_ptr = NULL;  // Release SRAM
    return true;
}

void card_cache_flush_to_flash(card_cache_t *cache) {
    // Write dirty entries to flash
    // Flash sector size = 4096 bytes on RP2350
    for (uint16_t i = 0; i < cache->entry_count; i++) {
        card_entry_t *e = &cache->entries[i];
        if (!(e->flags & CARD_FLAG_DIRTY)) continue;
        if (!e->sram_ptr) continue;

        // Calculate flash offset for this card
        // Simple sequential allocation
        if (e->flash_offset == 0) {
            // Assign next available flash slot
            static uint32_t next_flash_offset = 0;
            if (next_flash_offset == 0) {
                next_flash_offset = FLASH_CARD_DATA - FLASH_FIRMWARE_BASE;
            }
            uint32_t aligned = (e->bytecode_len + 4095) & ~4095u;
            e->flash_offset = next_flash_offset;
            next_flash_offset += aligned;
        }

        platform_flash_erase_sector(e->flash_offset);
        platform_flash_write(e->flash_offset, e->sram_ptr, e->bytecode_len);
        e->flags &= ~CARD_FLAG_DIRTY;
    }
}

void card_cache_warm_from_flash(card_cache_t *cache) {
    // Read card index from flash and populate entries
    // Index format: [count:2][entries: major,minor,version,offset,len × N]
    uint8_t idx_buf[FLASH_CARD_INDEX_SIZE];
    platform_flash_read(FLASH_CARD_INDEX - FLASH_FIRMWARE_BASE, idx_buf, sizeof(idx_buf));

    uint16_t count;
    memcpy(&count, idx_buf, 2);
    if (count == 0 || count > CARD_CACHE_MAX_ENTRIES) return;

    uint32_t pos = 2;
    for (uint16_t i = 0; i < count && pos + 10 <= sizeof(idx_buf); i++) {
        card_entry_t *e = &cache->entries[cache->entry_count++];
        e->major = idx_buf[pos++];
        e->minor = idx_buf[pos++];
        memcpy(&e->version, idx_buf + pos, 2); pos += 2;
        memcpy(&e->flash_offset, idx_buf + pos, 4); pos += 4;
        memcpy(&e->bytecode_len, idx_buf + pos, 4); pos += 4;
        e->sram_ptr = NULL;  // Load on demand
        e->last_used = 0;
        e->use_count = 0;
        e->flags = 0;
    }

    // Pre-load first N cards into SRAM (hot start)
    for (uint16_t i = 0; i < cache->entry_count && i < 32; i++) {
        card_entry_t *e = &cache->entries[i];
        if (e->flash_offset == 0 || e->bytecode_len == 0) continue;
        uint32_t aligned = (e->bytecode_len + 3) & ~3u;
        uint8_t *ptr = pool_alloc(cache, aligned);
        if (!ptr) break;  // SRAM full
        platform_flash_read(e->flash_offset, ptr, e->bytecode_len);
        e->sram_ptr = ptr;
    }
}

void card_cache_get_stats(card_cache_t *cache, card_cache_stats_t *stats) {
    stats->total_cards = cache->entry_count;
    stats->sram_bytes_used = cache->sram_used;
    stats->sram_bytes_free = CARD_CACHE_SRAM_SIZE - cache->sram_used;
    stats->sram_resident = 0;
    stats->hits = 0;
    stats->misses = 0;
    for (uint16_t i = 0; i < cache->entry_count; i++) {
        if (cache->entries[i].sram_ptr) stats->sram_resident++;
    }
}
