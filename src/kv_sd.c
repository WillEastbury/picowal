#include "kv_sd.h"
#include "sd_card.h"
#include "httpd/web_server.h"
#include <string.h>
#include <stdio.h>

// ============================================================
// KV Store on SD — dynamic layout, bitmap allocator
// ============================================================

static const uint8_t SD_MAGIC[8] = { 0x31,0x41,0x59,0x26, 0x50,0x69,0x63,0x6F };

static kvsd_superblock_t g_sb;
static uint32_t g_index[KVSD_INDEX_MAX];       // sorted composite keys
static uint32_t g_index_count = 0;
static bool g_ready = false;
static uint8_t g_card_buf[KVSD_CARD_SIZE];

// Slot allocation: key→slot mapping via bitmap scan
// Cards stored at data_start + slot*KVSD_CARD_BLOCKS
// First 4 bytes of each SD card block stores the composite key for verification

// ============================================================
// Sorted index
// ============================================================

static int32_t index_find(uint32_t key) {
    int32_t lo = 0, hi = (int32_t)g_index_count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (g_index[mid] == key) return mid;
        if (g_index[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return -(lo + 1);
}

static bool index_insert(uint32_t key) {
    int32_t pos = index_find(key);
    if (pos >= 0) return true;
    if (g_index_count >= KVSD_INDEX_MAX) return false;
    uint32_t at = (uint32_t)(-(pos + 1));
    if (at < g_index_count) memmove(&g_index[at+1], &g_index[at], (g_index_count-at)*4);
    g_index[at] = key;
    g_index_count++;
    return true;
}

static bool index_remove(uint32_t key) {
    int32_t pos = index_find(key);
    if (pos < 0) return false;
    if ((uint32_t)pos < g_index_count-1) memmove(&g_index[pos], &g_index[pos+1], (g_index_count-(uint32_t)pos-1)*4);
    g_index_count--;
    return true;
}

// ============================================================
// Superblock
// ============================================================

static bool sb_read(void) {
    uint8_t buf[512];
    if (!sd_read_block(0, buf)) return false;
    if (memcmp(buf, SD_MAGIC, 8) != 0) return false;
    memcpy(&g_sb, buf, sizeof(g_sb));
    return true;
}

static bool sb_write(void) {
    uint8_t buf[512];
    memset(buf, 0, 512);
    memcpy(buf, &g_sb, sizeof(g_sb));
    return sd_write_block(0, buf);
}

// ============================================================
// Bitmap
// ============================================================

static bool bitmap_get(uint32_t slot) {
    uint32_t blk = g_sb.bitmap_start + (slot / 4096);
    uint32_t byte_off = (slot % 4096) / 8;
    uint8_t bit = 1u << (slot % 8);
    uint8_t buf[512];
    if (!sd_read_block(blk, buf)) return false;
    return (buf[byte_off] & bit) != 0;
}

static bool bitmap_set(uint32_t slot, bool used) {
    uint32_t blk = g_sb.bitmap_start + (slot / 4096);
    uint32_t byte_off = (slot % 4096) / 8;
    uint8_t bit = 1u << (slot % 8);
    uint8_t buf[512];
    if (!sd_read_block(blk, buf)) return false;
    if (used) buf[byte_off] |= bit; else buf[byte_off] &= ~bit;
    return sd_write_block(blk, buf);
}

// ============================================================
// Data addressing: slot N -> blocks (data_start + N*4)
// ============================================================

static uint32_t slot_to_block(uint32_t slot) { return g_sb.data_start + slot * KVSD_CARD_BLOCKS; }

static bool read_card(uint32_t key, uint8_t *buf) {
    return sd_read_blocks(slot_to_block(key), buf, KVSD_CARD_BLOCKS);
}

static bool write_card(uint32_t key, const uint8_t *buf) {
    return sd_write_blocks(slot_to_block(key), buf, KVSD_CARD_BLOCKS);
}

// ============================================================
// Format — compute layout from SD size
// ============================================================

static void format_sd(uint32_t total_blocks) {
    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.magic, SD_MAGIC, 8);
    g_sb.version = 1;
    g_sb.total_cards = 0;

    // Region sizes
    // Block 0: superblock, blocks 1-1024: OTA staging (512KB)
    uint32_t reserved = 1 + KVSD_OTA_BLOCKS;  // superblock + OTA
    g_sb.ota_start = 1;
    g_sb.ota_blocks = KVSD_OTA_BLOCKS;

    uint32_t available = total_blocks - reserved;
    uint32_t index_total = available * 15 / 100;  // 15% for index
    uint32_t data_total = available - index_total;

    // Index sub-regions
    uint32_t max_cards = data_total / KVSD_CARD_BLOCKS;
    uint32_t bitmap_blks = (max_cards + 4095) / 4096;  // 1 bit per card, 4096 per block
    uint32_t keylist_blks = (max_cards * 4 + 511) / 512;  // 4 bytes per key
    if (keylist_blks > index_total / 2) keylist_blks = index_total / 2;  // cap at half
    uint32_t bloom_blks = index_total - bitmap_blks - keylist_blks;

    g_sb.index_start = reserved;
    g_sb.index_blocks = index_total;
    g_sb.bitmap_start = reserved;
    g_sb.bitmap_blocks = bitmap_blks;
    g_sb.keylist_start = reserved + bitmap_blks;
    g_sb.keylist_blocks = keylist_blks;
    g_sb.bloom_start = g_sb.keylist_start + keylist_blks;
    g_sb.bloom_blocks = bloom_blks;
    g_sb.data_start = reserved + index_total;
    g_sb.data_blocks = data_total;
    g_sb.max_cards = max_cards;
    g_sb.next_free_hint = 0;
    g_sb.dirty = 0;

    web_log("[kvsd] Format: %lu total blocks\n", (unsigned long)total_blocks);
    web_log("[kvsd]   OTA:   %lu blocks @ %lu (512KB staging)\n",
            (unsigned long)g_sb.ota_blocks, (unsigned long)g_sb.ota_start);
    web_log("[kvsd]   Index: %lu blocks @ %lu (bitmap=%lu, keylist=%lu, bloom=%lu)\n",
            (unsigned long)index_total, (unsigned long)g_sb.index_start,
            (unsigned long)bitmap_blks, (unsigned long)keylist_blks, (unsigned long)bloom_blks);
    web_log("[kvsd]   Data:  %lu blocks @ %lu, max %lu cards\n",
            (unsigned long)data_total, (unsigned long)g_sb.data_start, (unsigned long)max_cards);

    sb_write();
}

// ============================================================
// Init
// ============================================================

void kvsd_init(void) {
    g_index_count = 0;
    g_ready = false;

    sd_info_t info;
    if (!sd_get_info(&info)) { web_log("[kvsd] SD not ready\n"); return; }
    web_log("[kvsd] SD: %lu MB, %lu blocks\n", (unsigned long)info.capacity_mb, (unsigned long)info.block_count);

    if (sb_read()) {
        web_log("[kvsd] Superblock found — %lu cards, data@%lu, max=%lu\n",
                (unsigned long)g_sb.total_cards, (unsigned long)g_sb.data_start,
                (unsigned long)g_sb.max_cards);

        // Try to load sorted key list from keylist region (fast boot)
        uint32_t saved = g_sb.total_cards;
        if (saved > 0 && saved <= KVSD_INDEX_MAX && g_sb.keylist_blocks > 0) {
            web_log("[kvsd] Loading %lu keys from keylist...\n", (unsigned long)saved);
            uint32_t loaded = 0;
            for (uint32_t b = 0; b < g_sb.keylist_blocks && loaded < saved; b++) {
                uint8_t buf[512];
                if (!sd_read_block(g_sb.keylist_start + b, buf)) break;
                for (uint32_t k = 0; k < 128 && loaded < saved; k++) {
                    uint32_t key = (uint32_t)buf[k*4] | ((uint32_t)buf[k*4+1]<<8) |
                                   ((uint32_t)buf[k*4+2]<<16) | ((uint32_t)buf[k*4+3]<<24);
                    if (key == 0xFFFFFFFF) break;
                    g_index[loaded++] = key;
                }
            }
            g_index_count = loaded;
            web_log("[kvsd] Loaded %lu keys\n", (unsigned long)loaded);
        }

        // If keylist was empty/corrupt, fall back to bitmap scan
        if (g_index_count == 0 && g_sb.total_cards > 0) {
            web_log("[kvsd] Keylist empty, scanning bitmap...\n");
            for (uint32_t bm = 0; bm < g_sb.bitmap_blocks && g_index_count < KVSD_INDEX_MAX; bm++) {
                uint8_t buf[512];
                if (!sd_read_block(g_sb.bitmap_start + bm, buf)) continue;
                bool all_zero = true;
                for (int i = 0; i < 512; i++) { if (buf[i]) { all_zero = false; break; } }
                if (all_zero) continue;
                uint32_t base = bm * 4096;
                for (uint32_t by = 0; by < 512 && g_index_count < KVSD_INDEX_MAX; by++) {
                    if (!buf[by]) continue;
                    for (uint8_t bi = 0; bi < 8; bi++) {
                        if (buf[by] & (1u<<bi)) g_index[g_index_count++] = base + by*8 + bi;
                    }
                }
            }
            web_log("[kvsd] Bitmap scan: %lu keys\n", (unsigned long)g_index_count);
        }
    } else {
        web_log("[kvsd] No superblock — formatting\n");
        format_sd(info.block_count);
    }

    g_ready = true;
    web_log("[kvsd] Ready (index: %lu/%lu, max cards: %lu)\n",
            (unsigned long)g_index_count, (unsigned long)KVSD_INDEX_MAX,
            (unsigned long)g_sb.max_cards);
}

// ============================================================
// Slot allocation — find free slot via bitmap
// ============================================================

static int32_t alloc_slot(void) {
    uint32_t hint = g_sb.next_free_hint;
    for (uint32_t tries = 0; tries < g_sb.max_cards; tries++) {
        uint32_t slot = (hint + tries) % g_sb.max_cards;
        if (!bitmap_get(slot)) {
            g_sb.next_free_hint = (slot + 1) % g_sb.max_cards;
            return (int32_t)slot;
        }
    }
    return -1; // full
}

// Find the slot for a given key by reading the stored key header
// Uses the SRAM index to narrow down — we store (key, slot) pairs
// in a secondary mapping that fits in the last 4 bytes of each card
static int32_t find_slot_for_key(uint32_t key) {
    uint32_t card_id = key & 0x3FFFFF;
    if (card_id < g_sb.max_cards) {
        // Fast path: card_id as slot, key stored at bytes 508-511 of first block
        uint8_t blk[512];
        if (sd_read_block(slot_to_block(card_id), blk)) {
            uint32_t stored_key = blk[508] | ((uint32_t)blk[509]<<8) |
                                  ((uint32_t)blk[510]<<16) | ((uint32_t)blk[511]<<24);
            if (stored_key == key) return (int32_t)card_id;
        }
    }
    return -1;
}

// ============================================================
// Put — with slot allocation
// ============================================================

bool kvsd_put(uint32_t key, const uint8_t *value, uint16_t len) {
    if (!g_ready || len > KVSD_CARD_SIZE - 4) return false;

    // Check if key already exists (update case)
    int32_t existing = find_slot_for_key(key);
    uint32_t slot;

    if (existing >= 0) {
        slot = (uint32_t)existing;
    } else {
        // Try card_id as slot first (keeps sequential)
        uint32_t card_id = key & 0x3FFFFF;
        if (card_id < g_sb.max_cards && !bitmap_get(card_id)) {
            slot = card_id;
        } else {
            int32_t s = alloc_slot();
            if (s < 0) return false;
            slot = (uint32_t)s;
        }
    }

    // Build card: [original data][key in last 4 bytes of first block]
    uint8_t card[KVSD_CARD_SIZE];
    memset(card, 0, KVSD_CARD_SIZE);
    memcpy(card, value, len);
    if (card[0] != KVSD_MAGIC_LO || card[1] != KVSD_MAGIC_HI) return false;
    // Store composite key at bytes 508-511 of first block for find_slot_for_key
    card[508] = (uint8_t)(key);
    card[509] = (uint8_t)(key >> 8);
    card[510] = (uint8_t)(key >> 16);
    card[511] = (uint8_t)(key >> 24);

    if (!write_card(slot, card)) return false;

    bool is_new = (index_find(key) < 0);
    if (is_new) {
        bitmap_set(slot, true);
        g_sb.total_cards++;
        g_sb.dirty = 1;
    }
    index_insert(key);
    return true;
}

// ============================================================
// Get
// ============================================================

const uint8_t *kvsd_get(uint32_t key, uint16_t *len) {
    if (!g_ready) return NULL;
    int32_t slot = find_slot_for_key(key);
    if (slot < 0) return NULL;
    if (!read_card((uint32_t)slot, g_card_buf)) return NULL;
    if (g_card_buf[0] != KVSD_MAGIC_LO || g_card_buf[1] != KVSD_MAGIC_HI) return NULL;
    // Trim trailing zeros up to byte 508 (key footer at 508-511)
    uint16_t l = 508;
    while (l > 4 && g_card_buf[l-1] == 0) l--;
    if (len) *len = l;
    return g_card_buf;
}

bool kvsd_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    if (!g_ready) return false;
    int32_t slot = find_slot_for_key(key);
    if (slot < 0) return false;
    uint8_t card[KVSD_CARD_SIZE];
    if (!read_card((uint32_t)slot, card)) return false;
    if (card[0] != KVSD_MAGIC_LO || card[1] != KVSD_MAGIC_HI) return false;
    uint16_t l = 508;
    while (l > 4 && card[l-1] == 0) l--;
    if (*len < l) return false;
    memcpy(out, card, l);
    *len = l;
    if (version) *version = (uint16_t)card[2] | ((uint16_t)card[3]<<8);
    return true;
}

// ============================================================
// Delete
// ============================================================

bool kvsd_delete(uint32_t key) {
    if (!g_ready) return false;
    int32_t slot = find_slot_for_key(key);
    if (slot < 0) return false;
    uint8_t empty[KVSD_CARD_SIZE];
    memset(empty, 0, KVSD_CARD_SIZE);
    write_card((uint32_t)slot, empty);
    bitmap_set((uint32_t)slot, false);
    if (index_remove(key)) { g_sb.total_cards--; g_sb.dirty = 1; }
    return true;
}

bool kvsd_exists(uint32_t key) {
    if (!g_ready) return false;
    return index_find(key) >= 0;
}

// ============================================================
// Range
// ============================================================

uint32_t kvsd_range(uint32_t prefix, uint32_t mask,
                    uint32_t *out_keys, uint16_t *out_unused, uint32_t max) {
    (void)out_unused;
    if (!g_ready) return 0;
    uint32_t masked = prefix & mask;
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_index_count && count < max; i++) {
        if ((g_index[i] & mask) == masked) out_keys[count++] = g_index[i];
        else if (g_index[i] > (masked | ~mask)) break;
    }
    return count;
}

// ============================================================
// Flush — persist index to SD
// ============================================================

bool kvsd_flush(void) {
    if (!g_ready) return false;

    // Write sorted key array to keylist region
    uint32_t keys_to_save = g_index_count;
    uint32_t blocks_needed = (keys_to_save * 4 + 511) / 512;
    if (blocks_needed > g_sb.keylist_blocks) blocks_needed = g_sb.keylist_blocks;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint8_t buf[512];
        memset(buf, 0xFF, 512);
        uint32_t start = b * 128;
        for (uint32_t k = 0; k < 128 && start+k < keys_to_save; k++) {
            uint32_t key = g_index[start+k];
            buf[k*4]=(uint8_t)key; buf[k*4+1]=(uint8_t)(key>>8);
            buf[k*4+2]=(uint8_t)(key>>16); buf[k*4+3]=(uint8_t)(key>>24);
        }
        if (!sd_write_block(g_sb.keylist_start + b, buf)) return false;
    }

    // Clear remaining keylist blocks (mark end)
    if (blocks_needed < g_sb.keylist_blocks) {
        uint8_t ff[512]; memset(ff, 0xFF, 512);
        sd_write_block(g_sb.keylist_start + blocks_needed, ff);
    }

    g_sb.dirty = 0;
    sb_write();

    web_log("[kvsd] Flushed %lu keys + superblock\n", (unsigned long)keys_to_save);
    return true;
}

// ============================================================
// Stats
// ============================================================

kvsd_stats_t kvsd_stats(void) {
    kvsd_stats_t st = {0};
    st.active = g_index_count;
    st.index_max = KVSD_INDEX_MAX;
    st.max_cards = g_sb.max_cards;
    sd_info_t info;
    if (sd_get_info(&info)) st.sd_mb = info.capacity_mb;
    return st;
}

uint32_t kvsd_record_count(void) { return g_index_count; }

uint32_t kvsd_type_counts(uint16_t *out_types, uint32_t *out_counts, uint32_t max_types) {
    uint32_t n = 0;
    uint16_t last = 0xFFFF;
    for (uint32_t i = 0; i < g_index_count; i++) {
        uint16_t t = (uint16_t)(g_index[i] >> 22);
        if (t == last) { out_counts[n-1]++; }
        else { if (n >= max_types) break; out_types[n]=t; out_counts[n]=1; n++; last=t; }
    }
    return n;
}

bool kvsd_ready(void) { return g_ready; }

uint32_t kvsd_ota_start_block(void) {
    if (!g_ready) return 0;
    // Backward compat: old superblocks may have ota_start=0
    if (g_sb.ota_start == 0 || g_sb.ota_blocks == 0) return 1; // default
    return g_sb.ota_start;
}
