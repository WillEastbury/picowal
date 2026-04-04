#include "storage.h"
#include "sd_card.h"
#include "field_index.h"
#include "httpd/web_server.h"
#include "kv_flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// Heatshrink — use our config
#define HEATSHRINK_DYNAMIC_ALLOC 0
#define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 64
#define HEATSHRINK_STATIC_WINDOW_BITS 8
#define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4
#define HEATSHRINK_DEBUGGING_LOGS 0
#define HEATSHRINK_USE_INDEX 0
#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"

#include <string.h>
#include <stdio.h>

// ============================================================
// Magic: first 8 digits of Pi + "Pico"
// ============================================================
static const uint8_t STORE_MAGIC[8] = {0x31,0x41,0x59,0x26,0x50,0x69,0x63,0x6F};

// ============================================================
// Superblock (persisted to SD block 0)
// ============================================================
typedef struct __attribute__((packed)) {
    uint8_t  magic[8];
    uint32_t version;
    uint32_t total_cards;
    uint32_t sd_total_blocks;

    // Index region on SD
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t bloom_start, bloom_blocks;
    uint32_t fieldidx_start, fieldidx_blocks;
    uint32_t reverse_start, reverse_blocks;
    uint32_t timestamp_start, timestamp_blocks;

    // Data region on SD
    uint32_t data_start, data_blocks;
    uint32_t max_cards;

    // Append pointer
    uint32_t append_block;      // next block to write to
    uint16_t append_offset;     // byte offset within that block
    uint16_t _pad;

    // Flash index info
    uint32_t locator_count;     // entries in CoreLocator on flash
    uint32_t freespace_count;   // entries in free-space index on flash

    uint32_t dirty;             // bitmask: which indexes need flush
    uint32_t timestamp_tail;    // next write position in timestamp log
} store_superblock_t;

// ============================================================
// Flash regions (internal flash)
// ============================================================
#define FLASH_LOCATOR_START   0x100000   // CoreLocator
#define FLASH_LOCATOR_SIZE    0x080000   // 512KB = ~42K entries
#define FLASH_FREESPACE_START 0x180000   // Free-space index
#define FLASH_FREESPACE_SIZE  0x080000   // 512KB = ~65K entries
#define FLASH_XIP_BASE        0x10000000

// XIP pointers for reading flash indexes
#define LOCATOR_XIP   ((const store_locator_t *)(FLASH_XIP_BASE + FLASH_LOCATOR_START))
#define FREESPACE_XIP ((const store_freespace_t *)(FLASH_XIP_BASE + FLASH_FREESPACE_START))

// ============================================================
// SRAM state
// ============================================================
static store_superblock_t g_sb;
static bool g_ready = false;

// Pack summary cache
static store_pack_summary_t g_packs[STORE_MAX_PACKS];
static uint32_t g_pack_count = 0;

// Histogram (derived from locator, cached)
static uint32_t g_histogram[1024]; // card count per pack ordinal (10-bit)

// Dirty locator buffer — new/changed entries since last flash flush
#define DIRTY_LOCATOR_MAX 512
static store_locator_t g_dirty_locator[DIRTY_LOCATOR_MAX];
static uint32_t g_dirty_locator_count = 0;

// Dirty freespace buffer
#define DIRTY_FREESPACE_MAX 256
static store_freespace_t g_dirty_freespace[DIRTY_FREESPACE_MAX];
static uint32_t g_dirty_freespace_count = 0;

// Heatshrink static instances
static heatshrink_encoder g_hs_enc;
static heatshrink_decoder g_hs_dec;

// Block I/O buffer
static uint8_t g_blk_buf[STORE_BLOCK_SIZE];

// ============================================================
// Compression helpers
// ============================================================

static uint16_t compress(const uint8_t *in, uint16_t in_len,
                         uint8_t *out, uint16_t out_max) {
    heatshrink_encoder_reset(&g_hs_enc);
    size_t sunk = 0, polled = 0;
    uint16_t out_len = 0;

    while (sunk < in_len) {
        size_t n = 0;
        heatshrink_encoder_sink(&g_hs_enc, &in[sunk], in_len - sunk, &n);
        sunk += n;

        HSE_poll_res pres;
        do {
            size_t pn = 0;
            pres = heatshrink_encoder_poll(&g_hs_enc, &out[out_len], out_max - out_len, &pn);
            out_len += (uint16_t)pn;
        } while (pres == HSER_POLL_MORE && out_len < out_max);
    }

    heatshrink_encoder_finish(&g_hs_enc);
    HSE_poll_res pres;
    do {
        size_t pn = 0;
        pres = heatshrink_encoder_poll(&g_hs_enc, &out[out_len], out_max - out_len, &pn);
        out_len += (uint16_t)pn;
    } while (pres == HSER_POLL_MORE && out_len < out_max);

    return out_len;
}

static uint16_t decompress(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t out_max) {
    heatshrink_decoder_reset(&g_hs_dec);
    size_t sunk = 0;
    uint16_t out_len = 0;

    while (sunk < in_len) {
        size_t n = 0;
        heatshrink_decoder_sink(&g_hs_dec, &in[sunk], in_len - sunk, &n);
        sunk += n;

        HSD_poll_res pres;
        do {
            size_t pn = 0;
            pres = heatshrink_decoder_poll(&g_hs_dec, &out[out_len], out_max - out_len, &pn);
            out_len += (uint16_t)pn;
        } while (pres == HSDR_POLL_MORE && out_len < out_max);
    }

    heatshrink_decoder_finish(&g_hs_dec);
    HSD_poll_res pres;
    do {
        size_t pn = 0;
        pres = heatshrink_decoder_poll(&g_hs_dec, &out[out_len], out_max - out_len, &pn);
        out_len += (uint16_t)pn;
    } while (pres == HSDR_POLL_MORE && out_len < out_max);

    return out_len;
}

// ============================================================
// Superblock I/O
// ============================================================

static bool sb_read(void) {
    uint8_t buf[512];
    if (!sd_read_block(0, buf)) return false;
    if (memcmp(buf, STORE_MAGIC, 8) != 0) return false;
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
// CoreLocator — binary search on flash (XIP)
// ============================================================

static int32_t locator_find(uint32_t key) {
    const store_locator_t *loc = LOCATOR_XIP;
    int32_t lo = 0, hi = (int32_t)g_sb.locator_count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (loc[mid].key == key) return mid;
        if (loc[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    // Check dirty buffer
    for (uint32_t i = 0; i < g_dirty_locator_count; i++) {
        if (g_dirty_locator[i].key == key) return -2 - (int32_t)i; // dirty hit
    }
    return -(lo + 1);
}

static const store_locator_t *locator_get(uint32_t key) {
    int32_t pos = locator_find(key);
    if (pos >= 0) return &LOCATOR_XIP[pos];
    if (pos <= -2) return &g_dirty_locator[-(pos + 2)];
    return NULL;
}

static void locator_dirty_add(uint32_t key, uint32_t block, uint16_t offset, uint16_t clen) {
    // Check if already in dirty buffer
    for (uint32_t i = 0; i < g_dirty_locator_count; i++) {
        if (g_dirty_locator[i].key == key) {
            g_dirty_locator[i].block = block;
            g_dirty_locator[i].offset = offset;
            g_dirty_locator[i].compressed_len = clen;
            return;
        }
    }
    if (g_dirty_locator_count < DIRTY_LOCATOR_MAX) {
        g_dirty_locator[g_dirty_locator_count++] = (store_locator_t){
            .key = key, .block = block, .offset = offset, .compressed_len = clen
        };
    }
}

// ============================================================
// Free-space — find a block with enough room
// ============================================================

static int32_t freespace_find_fit(uint16_t need) {
    // Check dirty buffer first (most recent updates)
    for (uint32_t i = 0; i < g_dirty_freespace_count; i++) {
        if (g_dirty_freespace[i].free_bytes >= need) return -2 - (int32_t)i;
    }
    // Check flash
    const store_freespace_t *fs = FREESPACE_XIP;
    for (uint32_t i = 0; i < g_sb.freespace_count; i++) {
        if (fs[i].free_bytes >= need) return (int32_t)i;
    }
    return -1; // no fit, use append
}

static void freespace_update(uint32_t block, uint16_t free_bytes) {
    // Update in dirty buffer
    for (uint32_t i = 0; i < g_dirty_freespace_count; i++) {
        if (g_dirty_freespace[i].block == block) {
            if (free_bytes == 512 || free_bytes == 0) {
                // Remove entry (block is empty or full)
                memmove(&g_dirty_freespace[i], &g_dirty_freespace[i+1],
                        (g_dirty_freespace_count - i - 1) * sizeof(store_freespace_t));
                g_dirty_freespace_count--;
            } else {
                g_dirty_freespace[i].free_bytes = free_bytes;
            }
            return;
        }
    }
    // New entry
    if (free_bytes > 0 && free_bytes < 512 && g_dirty_freespace_count < DIRTY_FREESPACE_MAX) {
        g_dirty_freespace[g_dirty_freespace_count++] = (store_freespace_t){
            .block = block, .free_bytes = free_bytes
        };
    }
}

// ============================================================
// Bitmap (on SD)
// ============================================================

static bool bitmap_set(uint32_t block, bool used) {
    uint32_t bm_block = g_sb.bitmap_start + (block / 4096);
    uint32_t byte_off = (block % 4096) / 8;
    uint8_t bit = 1u << (block % 8);
    uint8_t buf[512];
    if (!sd_read_block(bm_block, buf)) return false;
    if (used) buf[byte_off] |= bit; else buf[byte_off] &= ~bit;
    return sd_write_block(bm_block, buf);
}

// ============================================================
// Pack summary
// ============================================================

static void pack_summary_update_count(uint16_t pack_ord, int32_t delta) {
    for (uint32_t i = 0; i < g_pack_count; i++) {
        if (g_packs[i].pack_ord == pack_ord) {
            g_packs[i].card_count = (uint32_t)((int32_t)g_packs[i].card_count + delta);
            return;
        }
    }
    // New pack
    if (delta > 0 && g_pack_count < STORE_MAX_PACKS) {
        g_packs[g_pack_count].pack_ord = pack_ord;
        g_packs[g_pack_count].card_count = (uint32_t)delta;
        g_packs[g_pack_count].flags = 0;
        g_packs[g_pack_count].schema_key = 0;
        g_packs[g_pack_count].name[0] = '\0';
        g_pack_count++;
    }
}

// ============================================================
// Format
// ============================================================

bool store_format(void) {
    sd_info_t info;
    if (!sd_get_info(&info)) return false;

    uint32_t total = info.block_count;
    uint32_t reserved = 8;
    uint32_t avail = total - reserved;
    uint32_t index_total = avail * 15 / 100;
    uint32_t data_total = avail - index_total;

    // Divide index region
    uint32_t bitmap_blks = (data_total + 4095) / 4096;
    uint32_t remaining = index_total - bitmap_blks;
    uint32_t bloom_blks = remaining / 4;
    uint32_t fieldidx_blks = remaining / 4;
    uint32_t reverse_blks = remaining / 4;
    uint32_t timestamp_blks = remaining - bloom_blks - fieldidx_blks - reverse_blks;

    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.magic, STORE_MAGIC, 8);
    g_sb.version = 1;
    g_sb.sd_total_blocks = total;

    uint32_t off = reserved;
    g_sb.bitmap_start = off; g_sb.bitmap_blocks = bitmap_blks; off += bitmap_blks;
    g_sb.bloom_start = off; g_sb.bloom_blocks = bloom_blks; off += bloom_blks;
    g_sb.fieldidx_start = off; g_sb.fieldidx_blocks = fieldidx_blks; off += fieldidx_blks;
    g_sb.reverse_start = off; g_sb.reverse_blocks = reverse_blks; off += reverse_blks;
    g_sb.timestamp_start = off; g_sb.timestamp_blocks = timestamp_blks; off += timestamp_blks;

    g_sb.data_start = off;
    g_sb.data_blocks = total - off;
    g_sb.max_cards = g_sb.data_blocks; // theoretical max (packed, not 1:1)
    g_sb.append_block = off;
    g_sb.append_offset = 0;

    web_log("[store] Format: %lu blocks total\n", (unsigned long)total);
    web_log("[store]   Bitmap:    %lu blocks @ %lu\n", (unsigned long)bitmap_blks, (unsigned long)g_sb.bitmap_start);
    web_log("[store]   Bloom:     %lu blocks @ %lu\n", (unsigned long)bloom_blks, (unsigned long)g_sb.bloom_start);
    web_log("[store]   FieldIdx:  %lu blocks @ %lu\n", (unsigned long)fieldidx_blks, (unsigned long)g_sb.fieldidx_start);
    web_log("[store]   Reverse:   %lu blocks @ %lu\n", (unsigned long)reverse_blks, (unsigned long)g_sb.reverse_start);
    web_log("[store]   Timestamp: %lu blocks @ %lu\n", (unsigned long)timestamp_blks, (unsigned long)g_sb.timestamp_start);
    web_log("[store]   Data:      %lu blocks @ %lu\n", (unsigned long)g_sb.data_blocks, (unsigned long)g_sb.data_start);

    sb_write();
    return true;
}

// ============================================================
// Init
// ============================================================

bool store_init(void) {
    g_ready = false;
    g_pack_count = 0;
    g_dirty_locator_count = 0;
    g_dirty_freespace_count = 0;
    memset(g_histogram, 0, sizeof(g_histogram));

    sd_info_t info;
    if (!sd_get_info(&info)) {
        web_log("[store] SD not ready\n");
        return false;
    }

    if (sb_read()) {
        web_log("[store] Superblock OK — %lu cards, data@%lu\n",
                (unsigned long)g_sb.total_cards, (unsigned long)g_sb.data_start);

        // Load CoreLocator count from superblock — entries are on flash (XIP)
        web_log("[store] CoreLocator: %lu entries on flash\n",
                (unsigned long)g_sb.locator_count);

        // Rebuild histogram from locator
        const store_locator_t *loc = LOCATOR_XIP;
        for (uint32_t i = 0; i < g_sb.locator_count; i++) {
            uint16_t pack = (uint16_t)(loc[i].key >> 22);
            if (pack < 1024) g_histogram[pack]++;
        }

        // TODO: load pack summaries from SD
        // TODO: load bloom filters for hot packs

    } else {
        web_log("[store] No superblock — formatting\n");
        if (!store_format()) return false;
    }

    // Init field value index
    fidx_init(g_sb.fieldidx_start, g_sb.fieldidx_blocks);

    g_ready = true;
    web_log("[store] Ready\n");
    return true;
}

// ============================================================
// Put — compress, pack into block, update indexes
// ============================================================

bool store_put(uint32_t key, const uint8_t *data, uint16_t len) {
    if (!g_ready || len > STORE_MAX_RAW_SIZE) return false;

    // Compress
    uint8_t compressed[STORE_MAX_RAW_SIZE];
    uint16_t clen = compress(data, len, compressed, sizeof(compressed));

    // If compression didn't help, store raw
    bool is_compressed = (clen < len);
    const uint8_t *store_data = is_compressed ? compressed : data;
    uint16_t store_len = is_compressed ? clen : len;

    uint16_t total_need = STORE_RECORD_HDR_SIZE + store_len;

    // Find space — check free-space index for a block with room
    uint32_t target_block;
    uint16_t target_offset;

    int32_t fit = freespace_find_fit(total_need);
    if (fit >= 0) {
        // Fit in existing flash-indexed block
        target_block = FREESPACE_XIP[fit].block;
        // Read block to find actual write offset
        if (!sd_read_block(target_block, g_blk_buf)) return false;
        target_offset = STORE_BLOCK_SIZE - FREESPACE_XIP[fit].free_bytes;
    } else if (fit <= -2) {
        // Fit in dirty-buffered block
        uint32_t di = (uint32_t)(-(fit + 2));
        target_block = g_dirty_freespace[di].block;
        if (!sd_read_block(target_block, g_blk_buf)) return false;
        target_offset = STORE_BLOCK_SIZE - g_dirty_freespace[di].free_bytes;
    } else {
        // Append to new block
        target_block = g_sb.append_block;
        target_offset = g_sb.append_offset;

        if (target_offset == 0) {
            memset(g_blk_buf, 0xFF, STORE_BLOCK_SIZE);
        } else {
            if (!sd_read_block(target_block, g_blk_buf)) return false;
        }

        // If record doesn't fit in current append block, advance
        if (target_offset + total_need > STORE_BLOCK_SIZE) {
            // Current block is done — mark as full
            if (target_offset > 0 && target_offset < STORE_BLOCK_SIZE) {
                freespace_update(target_block, STORE_BLOCK_SIZE - target_offset);
            }
            target_block++;
            target_offset = 0;
            memset(g_blk_buf, 0xFF, STORE_BLOCK_SIZE);
            g_sb.append_block = target_block;
        }
    }

    // Write record into block buffer
    store_record_hdr_t hdr = {
        .key = key,
        .compressed_len = store_len,
        .raw_len = len
    };
    memcpy(g_blk_buf + target_offset, &hdr, STORE_RECORD_HDR_SIZE);
    memcpy(g_blk_buf + target_offset + STORE_RECORD_HDR_SIZE, store_data, store_len);

    // Write block to SD
    if (!sd_write_block(target_block, g_blk_buf)) return false;

    // Update free space
    uint16_t new_free = STORE_BLOCK_SIZE - (target_offset + total_need);
    freespace_update(target_block, new_free);

    // Update append pointer if we used it
    if (fit == -1) {
        g_sb.append_block = target_block;
        g_sb.append_offset = target_offset + total_need;
        if (g_sb.append_offset >= STORE_BLOCK_SIZE) {
            g_sb.append_block++;
            g_sb.append_offset = 0;
        }
    }

    // Update locator
    bool is_new = (locator_get(key) == NULL);
    locator_dirty_add(key, target_block, target_offset, store_len);

    // Update bitmap
    bitmap_set(target_block, true);

    // Update counts
    if (is_new) {
        g_sb.total_cards++;
        uint16_t pack = (uint16_t)(key >> 22);
        if (pack < 1024) g_histogram[pack]++;
        pack_summary_update_count(pack, 1);
    }

    g_sb.dirty |= 0x01; // locator dirty
    return true;
}

// ============================================================
// Get — find via locator, read block, decompress
// ============================================================

bool store_get(uint32_t key, uint8_t *out, uint16_t *len) {
    if (!g_ready) return false;

    const store_locator_t *loc = locator_get(key);
    if (!loc) return false;

    // Read the block containing this record
    if (!sd_read_block(loc->block, g_blk_buf)) return false;

    // Parse record header
    store_record_hdr_t hdr;
    memcpy(&hdr, g_blk_buf + loc->offset, STORE_RECORD_HDR_SIZE);
    if (hdr.key != key) return false;

    const uint8_t *compressed = g_blk_buf + loc->offset + STORE_RECORD_HDR_SIZE;

    if (hdr.compressed_len == hdr.raw_len) {
        // Not compressed
        if (*len < hdr.raw_len) return false;
        memcpy(out, compressed, hdr.raw_len);
        *len = hdr.raw_len;
    } else {
        // Decompress
        if (*len < hdr.raw_len) return false;
        uint16_t dlen = decompress(compressed, hdr.compressed_len, out, hdr.raw_len);
        *len = dlen;
    }
    return true;
}

// ============================================================
// Delete
// ============================================================

bool store_delete(uint32_t key) {
    if (!g_ready) return false;

    const store_locator_t *loc = locator_get(key);
    if (!loc) return false;

    // Zero out the record in the block
    if (sd_read_block(loc->block, g_blk_buf)) {
        memset(g_blk_buf + loc->offset, 0, STORE_RECORD_HDR_SIZE + loc->compressed_len);
        sd_write_block(loc->block, g_blk_buf);
    }

    // Update counts
    uint16_t pack = (uint16_t)(key >> 22);
    g_sb.total_cards--;
    if (pack < 1024 && g_histogram[pack] > 0) g_histogram[pack]--;
    pack_summary_update_count(pack, -1);

    // Mark locator entry as deleted (key=0 in dirty buffer)
    locator_dirty_add(key, 0, 0, 0);

    g_sb.dirty |= 0x01;
    return true;
}

bool store_exists(uint32_t key) {
    if (!g_ready) return false;
    const store_locator_t *loc = locator_get(key);
    return loc != NULL && loc->block != 0;
}

// ============================================================
// Range
// ============================================================

uint32_t store_range(uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint32_t max) {
    if (!g_ready) return 0;

    uint32_t masked = prefix & mask;
    uint32_t count = 0;

    // Scan flash locator
    const store_locator_t *loc = LOCATOR_XIP;
    for (uint32_t i = 0; i < g_sb.locator_count && count < max; i++) {
        if (loc[i].block == 0) continue; // deleted
        if ((loc[i].key & mask) == masked) out_keys[count++] = loc[i].key;
    }

    // Scan dirty buffer
    for (uint32_t i = 0; i < g_dirty_locator_count && count < max; i++) {
        if (g_dirty_locator[i].block == 0) continue;
        if ((g_dirty_locator[i].key & mask) == masked) {
            // Check not already in result
            bool dup = false;
            for (uint32_t j = 0; j < count; j++) {
                if (out_keys[j] == g_dirty_locator[i].key) { dup = true; break; }
            }
            if (!dup) out_keys[count++] = g_dirty_locator[i].key;
        }
    }

    // Sort results
    for (uint32_t i = 1; i < count; i++) {
        uint32_t k = out_keys[i]; uint32_t j = i;
        while (j > 0 && out_keys[j-1] > k) { out_keys[j] = out_keys[j-1]; j--; }
        out_keys[j] = k;
    }

    return count;
}

// ============================================================
// Pack summary
// ============================================================

const store_pack_summary_t *store_get_packs(uint32_t *count) {
    *count = g_pack_count;
    return g_packs;
}

bool store_update_pack_summary(uint16_t pack_ord, const char *name,
                               uint16_t flags, uint32_t schema_key) {
    for (uint32_t i = 0; i < g_pack_count; i++) {
        if (g_packs[i].pack_ord == pack_ord) {
            if (name) strncpy(g_packs[i].name, name, 47);
            g_packs[i].flags = flags;
            g_packs[i].schema_key = schema_key;
            return true;
        }
    }
    if (g_pack_count < STORE_MAX_PACKS) {
        g_packs[g_pack_count].pack_ord = pack_ord;
        g_packs[g_pack_count].flags = flags;
        g_packs[g_pack_count].schema_key = schema_key;
        if (name) strncpy(g_packs[g_pack_count].name, name, 47);
        g_pack_count++;
        return true;
    }
    return false;
}

// ============================================================
// Flush — persist dirty indexes
// ============================================================

static void __no_inline_not_in_flash_func(flash_write_sector)(
    uint32_t offset, const uint8_t *data, uint32_t len) {
    flash_range_erase(offset, 4096);
    for (uint32_t p = 0; p < len; p += 256)
        flash_range_program(offset + p, data + p, 256 > len - p ? 256 : len - p);
}

bool store_flush(void) {
    if (!g_ready) return false;

    if (g_dirty_locator_count > 0) {
        // Merge dirty locator entries into flash
        // For now: simple append to flash locator region
        // TODO: proper merge + compaction
        uint32_t flash_off = FLASH_LOCATOR_START + g_sb.locator_count * STORE_LOCATOR_SIZE;
        uint32_t sector_off = flash_off & ~0xFFF;  // align to 4KB sector

        uint8_t sector[4096];
        // Read existing sector via XIP
        memcpy(sector, (const uint8_t *)(FLASH_XIP_BASE + sector_off), 4096);

        // Append new entries
        for (uint32_t i = 0; i < g_dirty_locator_count; i++) {
            uint32_t entry_off = (flash_off + i * STORE_LOCATOR_SIZE) - sector_off;
            if (entry_off + STORE_LOCATOR_SIZE <= 4096) {
                memcpy(sector + entry_off, &g_dirty_locator[i], STORE_LOCATOR_SIZE);
            }
        }

        uint32_t irq = save_and_disable_interrupts();
        flash_write_sector(sector_off, sector, 4096);
        restore_interrupts(irq);

        g_sb.locator_count += g_dirty_locator_count;
        g_dirty_locator_count = 0;
        web_log("[store] Flushed locator: %lu entries on flash\n",
                (unsigned long)g_sb.locator_count);
    }

    // Write superblock
    g_sb.dirty = 0;
    sb_write();
    web_log("[store] Superblock flushed\n");
    return true;
}

// ============================================================
// Stats
// ============================================================

store_stats_t store_stats(void) {
    store_stats_t st = {0};
    st.total_cards = g_sb.total_cards;
    st.max_data_blocks = g_sb.data_blocks;
    st.locator_count = g_sb.locator_count + g_dirty_locator_count;
    st.freespace_count = g_sb.freespace_count + g_dirty_freespace_count;
    st.packs = g_pack_count;
    sd_info_t info;
    if (sd_get_info(&info)) st.sd_mb = info.capacity_mb;
    return st;
}

bool store_ready(void) { return g_ready; }

bool store_log_write(uint32_t key, uint8_t op) {
    // TODO: append to timestamp log region on SD
    (void)key; (void)op;
    return true;
}
