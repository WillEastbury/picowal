#include "field_index.h"
#include "sd_card.h"
#include "httpd/web_server.h"
#include <string.h>

// ============================================================
// Field Value Index — hash table on SD
// ============================================================

static uint32_t g_fidx_start = 0;   // SD block offset
static uint32_t g_fidx_blocks = 0;
static bool g_fidx_ready = false;

// SRAM bucket cache (LRU, 16 slots)
#define BUCKET_CACHE_SIZE 16
static struct {
    uint32_t bucket_id;
    uint8_t  data[512];
    bool     dirty;
    uint32_t access;    // LRU counter
} g_cache[BUCKET_CACHE_SIZE];
static uint32_t g_access_counter = 0;

// ============================================================
// FNV-1a hash
// ============================================================

uint32_t fidx_hash(const uint8_t *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

// Composite hash: pack + field + value → bucket
static uint32_t compute_bucket(uint16_t pack, uint8_t field_ord,
                               const uint8_t *value, uint16_t value_len) {
    uint8_t key[256];
    uint32_t klen = 0;
    key[klen++] = (uint8_t)pack;
    key[klen++] = (uint8_t)(pack >> 8);
    key[klen++] = field_ord;
    // For text: lowercase first 8 chars
    for (uint16_t i = 0; i < value_len && klen < 11; i++) {
        uint8_t c = value[i];
        if (c >= 'A' && c <= 'Z') c += 32; // lowercase
        key[klen++] = c;
    }
    return fidx_hash(key, klen) % FIDX_BUCKET_COUNT;
}

// Value hash: for exact match verification within bucket
static uint32_t hash_value(const uint8_t *value, uint16_t len) {
    return fidx_hash(value, len);
}

// ============================================================
// Bucket I/O with LRU cache
// ============================================================

static int cache_find(uint32_t bucket_id) {
    for (int i = 0; i < BUCKET_CACHE_SIZE; i++) {
        if (g_cache[i].bucket_id == bucket_id && g_cache[i].access > 0)
            return i;
    }
    return -1;
}

static int cache_evict(void) {
    int oldest = 0;
    uint32_t oldest_access = UINT32_MAX;
    for (int i = 0; i < BUCKET_CACHE_SIZE; i++) {
        if (g_cache[i].access == 0) return i; // empty slot
        if (g_cache[i].access < oldest_access) {
            oldest_access = g_cache[i].access;
            oldest = i;
        }
    }
    // Flush if dirty
    if (g_cache[oldest].dirty) {
        sd_write_block(g_fidx_start + (g_cache[oldest].bucket_id % g_fidx_blocks),
                      g_cache[oldest].data);
    }
    g_cache[oldest].access = 0;
    return oldest;
}

static uint8_t *bucket_read(uint32_t bucket_id) {
    int slot = cache_find(bucket_id);
    if (slot >= 0) {
        g_cache[slot].access = ++g_access_counter;
        return g_cache[slot].data;
    }

    slot = cache_evict();
    g_cache[slot].bucket_id = bucket_id;
    g_cache[slot].dirty = false;
    g_cache[slot].access = ++g_access_counter;

    uint32_t blk = g_fidx_start + (bucket_id % g_fidx_blocks);
    if (!sd_read_block(blk, g_cache[slot].data)) {
        memset(g_cache[slot].data, 0, 512);
    }
    return g_cache[slot].data;
}

static void bucket_mark_dirty(uint32_t bucket_id) {
    int slot = cache_find(bucket_id);
    if (slot >= 0) g_cache[slot].dirty = true;
}

static void cache_flush_all(void) {
    for (int i = 0; i < BUCKET_CACHE_SIZE; i++) {
        if (g_cache[i].dirty && g_cache[i].access > 0) {
            sd_write_block(g_fidx_start + (g_cache[i].bucket_id % g_fidx_blocks),
                          g_cache[i].data);
            g_cache[i].dirty = false;
        }
    }
}

// ============================================================
// Init
// ============================================================

void fidx_init(uint32_t sd_start_block, uint32_t sd_block_count) {
    g_fidx_start = sd_start_block;
    g_fidx_blocks = sd_block_count;
    g_fidx_ready = (sd_block_count > 0);
    memset(g_cache, 0, sizeof(g_cache));
    g_access_counter = 0;
    web_log("[fidx] Init: %lu blocks @ %lu, %s\n",
            (unsigned long)sd_block_count, (unsigned long)sd_start_block,
            g_fidx_ready ? "ready" : "disabled");
}

// ============================================================
// Insert
// ============================================================

bool fidx_insert(uint16_t pack, uint32_t card_id,
                 uint8_t field_ord, uint8_t value_type,
                 const uint8_t *value, uint16_t value_len) {
    if (!g_fidx_ready) return false;

    uint32_t bucket_id = compute_bucket(pack, field_ord, value, value_len);
    uint8_t *blk = bucket_read(bucket_id);

    fidx_bucket_hdr_t *hdr = (fidx_bucket_hdr_t *)blk;
    if (hdr->count >= FIDX_ENTRIES_PER_BLK) {
        // Bucket full — TODO: overflow chain
        return false;
    }

    fidx_entry_t entry = {
        .pack = pack,
        .field_ord = field_ord,
        .value_type = value_type,
        .value_hash = hash_value(value, value_len),
        .card_id = card_id
    };

    uint32_t off = 8 + hdr->count * FIDX_ENTRY_SIZE;
    memcpy(blk + off, &entry, FIDX_ENTRY_SIZE);
    hdr->count++;

    bucket_mark_dirty(bucket_id);
    return true;
}

// ============================================================
// Search (exact match) — O(1) bucket read + linear scan
// ============================================================

uint32_t fidx_search(uint16_t pack, uint8_t field_ord,
                     const uint8_t *value, uint16_t value_len,
                     uint32_t *out_card_ids, uint32_t max_results) {
    if (!g_fidx_ready) return 0;

    uint32_t bucket_id = compute_bucket(pack, field_ord, value, value_len);
    uint8_t *blk = bucket_read(bucket_id);

    fidx_bucket_hdr_t *hdr = (fidx_bucket_hdr_t *)blk;
    uint32_t vhash = hash_value(value, value_len);
    uint32_t count = 0;

    for (uint16_t i = 0; i < hdr->count && count < max_results; i++) {
        fidx_entry_t *e = (fidx_entry_t *)(blk + 8 + i * FIDX_ENTRY_SIZE);
        if (e->pack == pack && e->field_ord == field_ord && e->value_hash == vhash) {
            out_card_ids[count++] = e->card_id;
        }
    }
    return count;
}

// ============================================================
// Prefix search — hash first 3 chars, scan for prefix match
// ============================================================

uint32_t fidx_search_prefix(uint16_t pack, uint8_t field_ord,
                            const uint8_t *prefix, uint16_t prefix_len,
                            uint32_t *out_card_ids, uint32_t max_results) {
    if (!g_fidx_ready || prefix_len == 0) return 0;

    // Use first 3 chars (or less) for bucket lookup
    uint16_t hash_len = prefix_len < 3 ? prefix_len : 3;
    uint32_t bucket_id = compute_bucket(pack, field_ord, prefix, hash_len);
    uint8_t *blk = bucket_read(bucket_id);

    fidx_bucket_hdr_t *hdr = (fidx_bucket_hdr_t *)blk;
    uint32_t count = 0;

    // All entries in this bucket have the same first-3-char hash,
    // so they're all prefix candidates. We rely on the caller to
    // verify against actual card data if needed.
    for (uint16_t i = 0; i < hdr->count && count < max_results; i++) {
        fidx_entry_t *e = (fidx_entry_t *)(blk + 8 + i * FIDX_ENTRY_SIZE);
        if (e->pack == pack && e->field_ord == field_ord) {
            out_card_ids[count++] = e->card_id;
        }
    }
    return count;
}

// ============================================================
// Remove all entries for a card
// ============================================================

bool fidx_remove_card(uint16_t pack, uint32_t card_id) {
    // This is expensive — need to scan all buckets the card might be in.
    // In practice, we know which fields the card has from its schema,
    // so the caller should remove field by field.
    // For now, this is a no-op stub.
    (void)pack; (void)card_id;
    cache_flush_all();
    return true;
}
