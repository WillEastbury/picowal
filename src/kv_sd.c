#include "kv_sd.h"
#include "sd_card.h"
#include "httpd/web_server.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

// ============================================================
// KV Store on SD — dynamic layout, bitmap allocator
// ============================================================

// ============================================================
// Flash Index Tier (tier 2) — XIP-mapped sorted (key,slot) pairs
//
// Layout (with FIDX_ENTRY_OFFSET = 256):
//   FIDX_FLASH_OFFSET + 0   : fidx_region_hdr_t  (256 bytes, written LAST)
//   FIDX_FLASH_OFFSET + 256 : fidx_entry_t[N]    (8 bytes each, written first)
//
// Write ordering: entries are written to flash before the header is sealed.
// This ensures that if power is lost during a flush, the header is absent
// (or stale from the previous flush), fidx_count_entries() returns 0, and
// the flash index is safely ignored until the next successful flush.
// ============================================================

typedef struct __attribute__((packed)) {
    uint32_t key;
    uint32_t slot;
} fidx_entry_t;

// Monotonically increasing flush sequence — loaded from the header on boot,
// incremented each time fidx_write_all() seals a new snapshot.
static uint32_t g_fidx_sequence = 0;

static uint32_t g_fidx_count = 0;  // entries currently live in flash index

// CRC32 over an arbitrary byte range (standard IEEE 802.3 polynomial).
// Used both to compute the header CRC during flush and to validate it on boot.
static uint32_t crc32_range(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Read a flash index entry via XIP (zero-copy).
// Entries start at FIDX_ENTRY_OFFSET bytes into the FIDX region, after the header.
static inline const fidx_entry_t *fidx_xip(uint32_t idx) {
    return &((const fidx_entry_t *)(FIDX_XIP_BASE + FIDX_ENTRY_OFFSET))[idx];
}

// Binary search the flash index via XIP — O(log n), zero I/O
static int32_t fidx_find(uint32_t key) {
    if (g_fidx_count == 0) return -1;
    int32_t lo = 0, hi = (int32_t)g_fidx_count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        uint32_t mk = fidx_xip((uint32_t)mid)->key;
        if (mk == key) return mid;
        if (mk < key) lo = mid + 1; else hi = mid - 1;
    }
    return -(lo + 1);
}

// Look up a key in flash index, return slot or -1
static int32_t fidx_lookup(uint32_t key) {
    int32_t pos = fidx_find(key);
    if (pos >= 0) return (int32_t)fidx_xip((uint32_t)pos)->slot;
    return -1;
}

// fidx_count_entries: validate the FIDX region header on boot.
//
// Recovery algorithm (issue requirement §5 for tier-2 index):
//   1. Read the header at FIDX_FLASH_OFFSET.
//   2. Validate magic and version.
//   3. Compute CRC32 over all entry data and compare against hdr.crc32.
//      A mismatch means the entries were partially written (or bit-flipped);
//      the entire flash index is discarded (g_fidx_count = 0).
//   4. Use hdr.entry_count as the authoritative count — no scan needed.
//   5. Restore g_fidx_sequence so the next flush gets a higher number.
static void fidx_count_entries(void) {
    g_fidx_count = 0;
    const fidx_region_hdr_t *hdr = (const fidx_region_hdr_t *)FIDX_XIP_BASE;

    if (hdr->magic   != FIDX_MAGIC)   return;  // not initialised or wrong magic
    if (hdr->version != FIDX_VERSION) return;  // incompatible format
    if (hdr->entry_count > FIDX_MAX_ENTRIES) return;  // corrupt count field

    // Validate all entry data against the stored CRC.
    // CRC is computed over the raw bytes of the entry array so any bit-flip
    // in either keys or slots is detected before the data is used.
    const uint8_t *entries = (const uint8_t *)(FIDX_XIP_BASE + FIDX_ENTRY_OFFSET);
    uint32_t computed = crc32_range(entries, hdr->entry_count * FIDX_ENTRY_SIZE);
    if (computed != hdr->crc32) {
        // Entry data is corrupted or was partially written; discard the flash
        // index so the SRAM tier is rebuilt from the SD keylist on this boot.
        printf("[fidx] boot: CRC mismatch seq=%lu — discarding flash index\n",
               (unsigned long)hdr->sequence);
        return;
    }

    g_fidx_count    = hdr->entry_count;
    g_fidx_sequence = hdr->sequence;

    printf("[fidx] boot: %lu entries seq=%lu\n",
           (unsigned long)g_fidx_count, (unsigned long)g_fidx_sequence);
}

// fidx_write_all: flush the entire sorted SRAM index to the flash index tier.
//
// Write ordering (issue requirement §4):
//   1. Erase all FIDX sectors (header + entries both cleared).
//   2. Write entries starting at offset FIDX_ENTRY_OFFSET.
//      The header region (offset 0) remains 0xFF (erased) — unrecognisable.
//   3. Seal: write the header with incremented sequence and CRC over the entries.
//
// If power is lost between steps 2 and 3, the header is absent; the next boot
// returns g_fidx_count = 0 and the SRAM index is rebuilt from the SD keylist.
// If power is lost mid-erase (step 1), some sectors may be partially erased;
// the header CRC will not match the partial entry data — also safely discarded.
static void fidx_write_all(const uint32_t *keys, const uint32_t *slots, uint32_t count) {
    if (count > FIDX_MAX_ENTRIES) count = FIDX_MAX_ENTRIES;

    uint32_t irq = save_and_disable_interrupts();

    // Step 1: erase all sectors (this also clears any previous header).
    for (uint32_t s = 0; s < FIDX_SECTORS; s++) {
        flash_range_erase(FIDX_FLASH_OFFSET + s * FIDX_SECTOR_SIZE, FIDX_SECTOR_SIZE);
    }

    // Step 2: write entries starting at FIDX_ENTRY_OFFSET (32 entries per 256-byte page).
    uint8_t page[256];
    for (uint32_t i = 0; i < count; i += 32) {
        memset(page, 0xFF, 256);
        uint32_t batch = count - i;
        if (batch > 32) batch = 32;
        for (uint32_t j = 0; j < batch; j++) {
            uint32_t off = j * 8;
            uint32_t k = keys[i + j], sl = slots[i + j];
            page[off+0]=(uint8_t)k;    page[off+1]=(uint8_t)(k>>8);
            page[off+2]=(uint8_t)(k>>16); page[off+3]=(uint8_t)(k>>24);
            page[off+4]=(uint8_t)sl;   page[off+5]=(uint8_t)(sl>>8);
            page[off+6]=(uint8_t)(sl>>16); page[off+7]=(uint8_t)(sl>>24);
        }
        flash_range_program(FIDX_FLASH_OFFSET + FIDX_ENTRY_OFFSET + i * 8, page, 256);
    }

    // Step 3: seal with header (sequence + entry_count + CRC).
    // Only after this write is the flash index considered authoritative on boot.
    // CRC is computed over the XIP-mapped entry bytes using the shared crc32_range()
    // helper — same polynomial and byte layout as what was just written to flash.
    const uint8_t *written = (const uint8_t *)(FIDX_XIP_BASE + FIDX_ENTRY_OFFSET);
    uint32_t crc = crc32_range(written, count * FIDX_ENTRY_SIZE);

    uint8_t hdr_buf[256];
    memset(hdr_buf, 0xFF, 256);
    fidx_region_hdr_t *hdr = (fidx_region_hdr_t *)hdr_buf;
    hdr->magic       = FIDX_MAGIC;
    hdr->version     = FIDX_VERSION;
    memset(hdr->_pad, 0, sizeof(hdr->_pad));
    hdr->sequence    = ++g_fidx_sequence;  // strictly increasing across flushes
    hdr->entry_count = count;
    hdr->crc32       = crc;
    memset(hdr->_reserved, 0xFF, sizeof(hdr->_reserved));
    flash_range_program(FIDX_FLASH_OFFSET, hdr_buf, 256);

    restore_interrupts(irq);
    g_fidx_count = count;
}

static const uint8_t SD_MAGIC[8] = { 0x31,0x41,0x59,0x26, 0x50,0x69,0x63,0x6F };

static kvsd_superblock_t g_sb;
static uint32_t g_index[KVSD_INDEX_MAX];       // sorted composite keys
static uint32_t g_slots[KVSD_INDEX_MAX];       // parallel: SD slot for each key (COW)
static uint32_t g_index_count = 0;
static bool g_ready = false;
static uint8_t g_card_buf[KVSD_CARD_SIZE];

// Forward declarations
static int32_t alloc_slot(void);

// Block allocation: bitmap tracks data blocks, packed with compressed cards

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

static bool index_insert(uint32_t key, uint32_t slot) {
    int32_t pos = index_find(key);
    if (pos >= 0) { g_slots[pos] = slot; return true; }
    if (g_index_count >= KVSD_INDEX_MAX) {
        // Evict first entry (lowest key) — still in flash index tier
        memmove(&g_index[0], &g_index[1], (g_index_count-1)*4);
        memmove(&g_slots[0], &g_slots[1], (g_index_count-1)*4);
        g_index_count--;
        // Recalculate insertion point after eviction
        pos = index_find(key);
        if (pos >= 0) { g_slots[pos] = slot; return true; }
    }
    uint32_t at = (uint32_t)(-(pos + 1));
    if (at < g_index_count) {
        memmove(&g_index[at+1], &g_index[at], (g_index_count-at)*4);
        memmove(&g_slots[at+1], &g_slots[at], (g_index_count-at)*4);
    }
    g_index[at] = key;
    g_slots[at] = slot;
    g_index_count++;
    return true;
}

static bool index_remove(uint32_t key) {
    int32_t pos = index_find(key);
    if (pos < 0) return false;
    if ((uint32_t)pos < g_index_count-1) {
        memmove(&g_index[pos], &g_index[pos+1], (g_index_count-(uint32_t)pos-1)*4);
        memmove(&g_slots[pos], &g_slots[pos+1], (g_index_count-(uint32_t)pos-1)*4);
    }
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
// On-disk hash table — 2-choice hashing, O(1) SD lookups
// ============================================================

// Bucket layout: [crc16:2][count:2][pad:4] [key:4 slot:4] × 63
typedef struct __attribute__((packed)) {
    uint16_t crc16;
    uint16_t count;      // entries in this bucket (0..63)
    uint32_t _reserved;
} ht_bucket_hdr_t;

_Static_assert(sizeof(ht_bucket_hdr_t) == HT_BUCKET_HDR_SIZE, "bucket hdr size");

// CRC16-CCITT over bucket payload (everything after crc16 field)
static uint16_t ht_crc16(const uint8_t *data, uint32_t len) {
    uint16_t crc = HT_MAGIC_CRC_SEED;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// Two independent hash functions for 2-choice hashing
static uint32_t ht_hash1(uint32_t key) {
    key ^= key >> 16;
    key *= 0x45d9f3bu;
    key ^= key >> 16;
    return key;
}

static uint32_t ht_hash2(uint32_t key) {
    key ^= key >> 15;
    key *= 0x735a2d97u;
    key ^= key >> 15;
    return key;
}

// Read a hash bucket from SD, validate CRC. Returns false if read fails or CRC bad.
// On CRC failure, zeros the buffer (empty bucket) — self-healing on corruption.
static bool ht_read_bucket(uint32_t bucket_idx, uint8_t *buf) {
    if (bucket_idx >= g_sb.hashtab_blocks) return false;
    if (!sd_read_block(g_sb.hashtab_start + bucket_idx, buf)) return false;
    ht_bucket_hdr_t *hdr = (ht_bucket_hdr_t *)buf;
    uint16_t expected = ht_crc16(buf + 2, 510);
    if (hdr->crc16 != expected || hdr->count > HT_ENTRIES_PER_BKT) {
        // Corrupt or blank bucket — treat as empty (self-heal)
        memset(buf, 0, 512);
        return true;
    }
    return true;
}

// Write a hash bucket to SD, computing CRC before write.
static bool ht_write_bucket(uint32_t bucket_idx, uint8_t *buf) {
    if (bucket_idx >= g_sb.hashtab_blocks) return false;
    ht_bucket_hdr_t *hdr = (ht_bucket_hdr_t *)buf;
    hdr->crc16 = ht_crc16(buf + 2, 510);
    return sd_write_block(g_sb.hashtab_start + bucket_idx, buf);
}

// Search a bucket buffer for a key. Returns entry index (0..count-1) or -1.
static int ht_bucket_find(const uint8_t *buf, uint32_t key) {
    const ht_bucket_hdr_t *hdr = (const ht_bucket_hdr_t *)buf;
    const uint8_t *entries = buf + HT_BUCKET_HDR_SIZE;
    for (uint16_t i = 0; i < hdr->count; i++) {
        uint32_t ek = (uint32_t)entries[i*8] | ((uint32_t)entries[i*8+1]<<8) |
                      ((uint32_t)entries[i*8+2]<<16) | ((uint32_t)entries[i*8+3]<<24);
        if (ek == key) return (int)i;
    }
    return -1;
}

// Get slot for a key from a bucket. Returns slot or -1.
static int32_t ht_bucket_get_slot(const uint8_t *buf, uint32_t key) {
    int idx = ht_bucket_find(buf, key);
    if (idx < 0) return -1;
    const uint8_t *e = buf + HT_BUCKET_HDR_SIZE + (uint32_t)idx * 8 + 4;
    return (int32_t)((uint32_t)e[0] | ((uint32_t)e[1]<<8) |
                     ((uint32_t)e[2]<<16) | ((uint32_t)e[3]<<24));
}

// Lookup key in hash table: check both candidate buckets.
// Returns slot or -1. O(1) amortized (1-2 SD reads).
static int32_t ht_lookup(uint32_t key) {
    if (g_sb.hashtab_blocks == 0) return -1;
    uint8_t buf[512];
    uint32_t b1 = ht_hash1(key) % g_sb.hashtab_blocks;
    if (ht_read_bucket(b1, buf)) {
        int32_t slot = ht_bucket_get_slot(buf, key);
        if (slot >= 0) return slot;
    }
    uint32_t b2 = ht_hash2(key) % g_sb.hashtab_blocks;
    if (b2 == b1) return -1;  // both hashes hit same bucket, already checked
    if (ht_read_bucket(b2, buf)) {
        int32_t slot = ht_bucket_get_slot(buf, key);
        if (slot >= 0) return slot;
    }
    return -1;
}

// Insert or update key→slot in hash table.
// Tries bucket1 first (if key exists there or has space), then bucket2.
// Returns true on success, false if both buckets full (extremely unlikely with 2-choice).
static bool ht_insert(uint32_t key, uint32_t slot) {
    if (g_sb.hashtab_blocks == 0) return false;
    uint8_t buf[512];
    uint32_t b1 = ht_hash1(key) % g_sb.hashtab_blocks;
    uint32_t b2 = ht_hash2(key) % g_sb.hashtab_blocks;

    // Check bucket 1
    if (ht_read_bucket(b1, buf)) {
        int idx = ht_bucket_find(buf, key);
        if (idx >= 0) {
            // Update existing entry's slot
            uint8_t *e = buf + HT_BUCKET_HDR_SIZE + (uint32_t)idx * 8 + 4;
            e[0]=(uint8_t)slot; e[1]=(uint8_t)(slot>>8);
            e[2]=(uint8_t)(slot>>16); e[3]=(uint8_t)(slot>>24);
            return ht_write_bucket(b1, buf);
        }
        ht_bucket_hdr_t *hdr = (ht_bucket_hdr_t *)buf;
        if (hdr->count < HT_ENTRIES_PER_BKT) {
            // Append to bucket 1
            uint8_t *e = buf + HT_BUCKET_HDR_SIZE + hdr->count * 8;
            e[0]=(uint8_t)key; e[1]=(uint8_t)(key>>8);
            e[2]=(uint8_t)(key>>16); e[3]=(uint8_t)(key>>24);
            e[4]=(uint8_t)slot; e[5]=(uint8_t)(slot>>8);
            e[6]=(uint8_t)(slot>>16); e[7]=(uint8_t)(slot>>24);
            hdr->count++;
            return ht_write_bucket(b1, buf);
        }
    }

    // Bucket 1 full — try bucket 2
    if (b2 == b1) return false;
    if (ht_read_bucket(b2, buf)) {
        int idx = ht_bucket_find(buf, key);
        if (idx >= 0) {
            uint8_t *e = buf + HT_BUCKET_HDR_SIZE + (uint32_t)idx * 8 + 4;
            e[0]=(uint8_t)slot; e[1]=(uint8_t)(slot>>8);
            e[2]=(uint8_t)(slot>>16); e[3]=(uint8_t)(slot>>24);
            return ht_write_bucket(b2, buf);
        }
        ht_bucket_hdr_t *hdr = (ht_bucket_hdr_t *)buf;
        if (hdr->count < HT_ENTRIES_PER_BKT) {
            uint8_t *e = buf + HT_BUCKET_HDR_SIZE + hdr->count * 8;
            e[0]=(uint8_t)key; e[1]=(uint8_t)(key>>8);
            e[2]=(uint8_t)(key>>16); e[3]=(uint8_t)(key>>24);
            e[4]=(uint8_t)slot; e[5]=(uint8_t)(slot>>8);
            e[6]=(uint8_t)(slot>>16); e[7]=(uint8_t)(slot>>24);
            hdr->count++;
            return ht_write_bucket(b2, buf);
        }
    }

    return false;  // both buckets full — astronomically unlikely with 2-choice
}

// Remove key from hash table. Swap-with-last for O(1) removal.
static bool ht_remove(uint32_t key) {
    if (g_sb.hashtab_blocks == 0) return false;
    uint8_t buf[512];
    uint32_t buckets[2] = { ht_hash1(key) % g_sb.hashtab_blocks,
                            ht_hash2(key) % g_sb.hashtab_blocks };
    for (int b = 0; b < 2; b++) {
        if (b == 1 && buckets[1] == buckets[0]) break;
        if (!ht_read_bucket(buckets[b], buf)) continue;
        int idx = ht_bucket_find(buf, key);
        if (idx < 0) continue;
        ht_bucket_hdr_t *hdr = (ht_bucket_hdr_t *)buf;
        // Swap with last entry, decrement count
        if ((uint16_t)idx < hdr->count - 1) {
            uint8_t *dst = buf + HT_BUCKET_HDR_SIZE + (uint32_t)idx * 8;
            uint8_t *src = buf + HT_BUCKET_HDR_SIZE + (hdr->count - 1) * 8;
            memcpy(dst, src, 8);
        }
        memset(buf + HT_BUCKET_HDR_SIZE + (hdr->count - 1) * 8, 0, 8);
        hdr->count--;
        return ht_write_bucket(buckets[b], buf);
    }
    return false;
}

// ============================================================
// Packed data blocks — variable-density compressed card storage
//
// Block header (8 bytes):
//   [count:1][_pad:1][used:2][reserved:4]
//
// Card entries packed sequentially after header:
//   [key:4][comp_len:2][raw_len:2][compressed_data... comp_len bytes]
//
// 504 bytes available for entries. Typical ~80B card → ~50B compressed
// → 58B per entry → 8+ cards per block.
//
// Hash table maps key → block_number. Read block, scan for key.
// Bitmap tracks data blocks (1 bit each).
// "Open block" in superblock = current append target.
// ============================================================

typedef struct __attribute__((packed)) {
    uint8_t  count;       // entries in this block
    uint8_t  _pad;
    uint16_t used;        // total bytes used (incl header)
    uint32_t _reserved;
} packed_blk_hdr_t;

_Static_assert(sizeof(packed_blk_hdr_t) == PACKED_BLK_HDR, "packed_blk_hdr_t size");

typedef struct __attribute__((packed)) {
    uint32_t key;
    uint16_t comp_len;
    uint16_t raw_len;
} packed_entry_hdr_t;

_Static_assert(sizeof(packed_entry_hdr_t) == PACKED_ENTRY_HDR, "packed_entry_hdr_t size");

#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"

static heatshrink_encoder g_hs_enc;
static heatshrink_decoder g_hs_dec;

static uint16_t hs_compress(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max) {
    heatshrink_encoder_reset(&g_hs_enc);
    size_t sunk = 0, polled = 0, total_out = 0;
    while (sunk < in_len) {
        size_t n = 0;
        heatshrink_encoder_sink(&g_hs_enc, (uint8_t *)&in[sunk], in_len - sunk, &n);
        sunk += n;
    }
    heatshrink_encoder_finish(&g_hs_enc);
    HSE_poll_res pr;
    do {
        pr = heatshrink_encoder_poll(&g_hs_enc, &out[total_out],
                                      out_max - total_out, &polled);
        total_out += polled;
        if (total_out > out_max) return 0;
    } while (pr == HSER_POLL_MORE);
    return (uint16_t)total_out;
}

static uint16_t hs_decompress(const uint8_t *in, uint16_t in_len,
                              uint8_t *out, uint16_t out_max) {
    heatshrink_decoder_reset(&g_hs_dec);
    size_t sunk = 0, polled = 0, total_out = 0;
    while (sunk < in_len) {
        size_t n = 0;
        heatshrink_decoder_sink(&g_hs_dec, (uint8_t *)&in[sunk], in_len - sunk, &n);
        sunk += n;
        HSD_poll_res pr;
        do {
            pr = heatshrink_decoder_poll(&g_hs_dec, &out[total_out],
                                          out_max - total_out, &polled);
            total_out += polled;
            if (total_out > out_max) return 0;
        } while (pr == HSDR_POLL_MORE);
    }
    heatshrink_decoder_finish(&g_hs_dec);
    HSD_poll_res pr;
    do {
        pr = heatshrink_decoder_poll(&g_hs_dec, &out[total_out],
                                      out_max - total_out, &polled);
        total_out += polled;
    } while (pr == HSDR_POLL_MORE);
    return (uint16_t)total_out;
}

static uint32_t blk_addr(uint32_t blk_num) {
    return g_sb.data_start + blk_num;
}

// Read a packed block from SD. Returns false if read fails or header invalid.
static bool packed_read(uint32_t blk_num, uint8_t *buf) {
    if (!sd_read_block(blk_addr(blk_num), buf)) return false;
    const packed_blk_hdr_t *hdr = (const packed_blk_hdr_t *)buf;
    if (hdr->used > 512 || hdr->count > 60) {
        // Corrupt — treat as empty
        memset(buf, 0, 512);
        packed_blk_hdr_t *h = (packed_blk_hdr_t *)buf;
        h->used = PACKED_BLK_HDR;
    }
    return true;
}

// Find a card entry within a packed block. Returns offset or 0 if not found.
static uint16_t packed_find(const uint8_t *blk, uint32_t key) {
    const packed_blk_hdr_t *hdr = (const packed_blk_hdr_t *)blk;
    uint16_t off = PACKED_BLK_HDR;
    for (uint8_t i = 0; i < hdr->count; i++) {
        if (off + PACKED_ENTRY_HDR > hdr->used) break;
        const packed_entry_hdr_t *e = (const packed_entry_hdr_t *)(blk + off);
        if (e->key == key) return off;
        off += PACKED_ENTRY_HDR + e->comp_len;
    }
    return 0;
}

// Read and decompress a card from a packed block.
// Returns raw length, or 0 on failure.
static uint16_t packed_read_card(uint32_t blk_num, uint32_t key,
                                  uint8_t *out, uint16_t out_max) {
    uint8_t blk[512];
    if (!packed_read(blk_num, blk)) return 0;
    uint16_t off = packed_find(blk, key);
    if (off == 0) return 0;
    const packed_entry_hdr_t *e = (const packed_entry_hdr_t *)(blk + off);
    if (off + PACKED_ENTRY_HDR + e->comp_len > 512) return 0;
    return hs_decompress(blk + off + PACKED_ENTRY_HDR, e->comp_len, out, out_max);
}

// Remove a card from a packed block. Compacts remaining entries.
// Returns true if found and removed. Frees block in bitmap if now empty.
static bool packed_remove_card(uint32_t blk_num, uint32_t key) {
    uint8_t blk[512];
    if (!packed_read(blk_num, blk)) return false;
    uint16_t off = packed_find(blk, key);
    if (off == 0) return false;
    packed_blk_hdr_t *hdr = (packed_blk_hdr_t *)blk;
    const packed_entry_hdr_t *e = (const packed_entry_hdr_t *)(blk + off);
    uint16_t entry_size = PACKED_ENTRY_HDR + e->comp_len;
    uint16_t tail = hdr->used - off - entry_size;
    if (tail > 0) memmove(blk + off, blk + off + entry_size, tail);
    memset(blk + hdr->used - entry_size, 0, entry_size);
    hdr->used -= entry_size;
    hdr->count--;

    sd_write_block(blk_addr(blk_num), blk);

    // Free block if completely empty
    if (hdr->count == 0) {
        bitmap_set(blk_num, false);
        // If this was the open block, reset it
        if (blk_num == g_sb.open_block) {
            g_sb.open_used = PACKED_BLK_HDR;
            g_sb.open_count = 0;
        }
    }
    return true;
}

// Append a compressed card to the open block. If it doesn't fit,
// allocate a new block. Returns the block number, or UINT32_MAX on failure.
static uint32_t packed_append(uint32_t key, const uint8_t *comp, uint16_t clen,
                               uint16_t raw_len) {
    uint16_t entry_size = PACKED_ENTRY_HDR + clen;
    if (entry_size > PACKED_DATA_MAX) return UINT32_MAX;  // card too large

    // Check if open block has space
    uint8_t blk[512];
    if (g_sb.open_used + entry_size <= 512 && g_sb.open_count < 60) {
        // Fits in current open block
        if (!packed_read(g_sb.open_block, blk)) {
            memset(blk, 0, 512);
            packed_blk_hdr_t *h = (packed_blk_hdr_t *)blk;
            h->used = PACKED_BLK_HDR;
        }
    } else {
        // Open block full — allocate a new one
        int32_t new_blk = alloc_slot();
        if (new_blk < 0) return UINT32_MAX;
        bitmap_set((uint32_t)new_blk, true);
        g_sb.open_block = (uint32_t)new_blk;
        g_sb.open_used = PACKED_BLK_HDR;
        g_sb.open_count = 0;
        memset(blk, 0, 512);
        packed_blk_hdr_t *h = (packed_blk_hdr_t *)blk;
        h->used = PACKED_BLK_HDR;
    }

    // Append entry
    packed_blk_hdr_t *hdr = (packed_blk_hdr_t *)blk;
    packed_entry_hdr_t *e = (packed_entry_hdr_t *)(blk + hdr->used);
    e->key = key;
    e->comp_len = clen;
    e->raw_len = raw_len;
    memcpy(blk + hdr->used + PACKED_ENTRY_HDR, comp, clen);
    hdr->used += entry_size;
    hdr->count++;
    g_sb.open_used = hdr->used;
    g_sb.open_count = hdr->count;

    sd_write_block(blk_addr(g_sb.open_block), blk);
    return g_sb.open_block;
}

// ============================================================
// Format — compute layout from SD size
// ============================================================

static void format_sd(uint32_t total_blocks) {
    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.magic, SD_MAGIC, 8);
    g_sb.version = KVSD_SB_VERSION;  // v3: 1-block cards + hash table
    g_sb.total_cards = 0;

    // Reserve last 256 blocks for SD ring buffer (UDP overflow WAL)
    uint32_t usable = total_blocks - SDRING_GUARD_BLOCKS;

    // Block 0: superblock, blocks 1-1200: OTA staging (600KB)
    uint32_t reserved = 1 + KVSD_OTA_BLOCKS;
    g_sb.ota_start = 1;
    g_sb.ota_blocks = KVSD_OTA_BLOCKS;

    uint32_t available = usable - reserved;
    uint32_t index_total = available * 15 / 100;
    uint32_t data_total = available - index_total;

    // Bitmap tracks data blocks (1 bit per block).
    // max_cards is the block count — actual card capacity is higher with packing.
    uint32_t max_cards = data_total;  // 1 bitmap bit per data block
    uint32_t bitmap_blks = (max_cards + 4095) / 4096;

    // Hash table sized for estimated total cards (assume avg 4 cards/block).
    uint32_t est_total_cards = data_total * 4;
    uint32_t ht_blks = (est_total_cards + 31) / 32;
    uint32_t ht_max = index_total - bitmap_blks;
    if (ht_blks > ht_max) ht_blks = ht_max;

    g_sb.index_start = reserved;
    g_sb.index_blocks = index_total;
    g_sb.bitmap_start = reserved;
    g_sb.bitmap_blocks = bitmap_blks;
    g_sb.hashtab_start = reserved + bitmap_blks;
    g_sb.hashtab_blocks = ht_blks;
    g_sb._reserved_start = 0;
    g_sb._reserved_blocks = 0;
    g_sb.data_start = reserved + index_total;
    g_sb.data_blocks = data_total;
    g_sb.max_cards = max_cards;  // bitmap capacity = data block count
    g_sb.next_free_hint = 0;
    g_sb.dirty = 0;
    g_sb.open_block = 0;
    g_sb.open_used = PACKED_BLK_HDR;
    g_sb.open_count = 0;

    // Allocate block 0 as the initial open block
    bitmap_set(0, true);

    web_log("[kvsd] fmt v4: %lu data blks, ~%lux capacity, %lu ht buckets\n",
            (unsigned long)data_total, (unsigned long)4, (unsigned long)ht_blks);

    sb_write();
}

// ============================================================
// Init
// ============================================================

void kvsd_init(void) {
    g_index_count    = 0;
    g_ready          = false;
    g_fidx_count     = 0;
    g_fidx_sequence  = 0;

    sd_info_t info;
    if (!sd_get_info(&info)) { web_log("[kvsd] no SD\n"); return; }

    // Load flash index tier (tier 2) — XIP binary search, zero I/O
    fidx_count_entries();

    if (sb_read()) {
        // Version check: v3 = 1-block cards + hash table. Older versions need reformat.
        if (g_sb.version < KVSD_SB_VERSION) {
            web_log("[kvsd] v%lu superblock — reformatting for v%d\n",
                    (unsigned long)g_sb.version, KVSD_SB_VERSION);
            format_sd(info.block_count);
        }

        web_log("[kvsd] v2: %lu cards, max %lu, %lu ht buckets\n",
                (unsigned long)g_sb.total_cards, (unsigned long)g_sb.max_cards,
                (unsigned long)g_sb.hashtab_blocks);

        // With the on-disk hash table, we don't need to load all keys at boot.
        // The SRAM index starts empty and populates lazily via hash table hits
        // (promote-on-read in find_slot_for_key). Flash XIP is tier 2 cache.
        //
        // Fallback: if hash table is empty/corrupt but bitmap shows cards,
        // rebuild hash table from bitmap scan. This is slow but self-healing.
        if (g_sb.total_cards > 0 && g_sb.hashtab_blocks == 0) {
            web_log("[kvsd] WARN: no hash table region — bitmap scan fallback\n");
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
                        if (!(buf[by] & (1u<<bi))) continue;
                        uint32_t blk_num = base + by*8 + bi;
                        uint8_t blk[512];
                        if (!packed_read(blk_num, blk)) continue;
                        const packed_blk_hdr_t *ph = (const packed_blk_hdr_t *)blk;
                        uint16_t off = PACKED_BLK_HDR;
                        for (uint8_t ei = 0; ei < ph->count && g_index_count < KVSD_INDEX_MAX; ei++) {
                            if (off + PACKED_ENTRY_HDR > ph->used) break;
                            const packed_entry_hdr_t *pe = (const packed_entry_hdr_t *)(blk + off);
                            if (pe->key != 0 && pe->key != 0xFFFFFFFF) {
                                g_index[g_index_count] = pe->key;
                                g_slots[g_index_count] = blk_num;
                                g_index_count++;
                            }
                            off += PACKED_ENTRY_HDR + pe->comp_len;
                        }
                    }
                }
            }
            // Sort by key for binary search
            for (uint32_t i = 1; i < g_index_count; i++) {
                uint32_t k = g_index[i], s = g_slots[i];
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && g_index[j] > k) {
                    g_index[j+1] = g_index[j]; g_slots[j+1] = g_slots[j]; j--;
                }
                g_index[j+1] = k; g_slots[j+1] = s;
            }
            web_log("[kvsd] bitmap scan: %lu keys\n", (unsigned long)g_index_count);
        }
    } else {
        web_log("[kvsd] no superblock, formatting v4\n");
        format_sd(info.block_count);
    }

    g_ready = true;
    web_log("[kvsd] ready, sram=%lu fidx=%lu ht=%lu buckets\n",
            (unsigned long)g_index_count, (unsigned long)g_fidx_count,
            (unsigned long)g_sb.hashtab_blocks);
}

// ============================================================
// Slot allocation — find free slot via bitmap
// ============================================================

static int32_t alloc_slot(void) {
    uint32_t hint = g_sb.next_free_hint;
    uint8_t buf[512];
    uint32_t cached_blk = UINT32_MAX;  // no block cached

    for (uint32_t tries = 0; tries < g_sb.max_cards; tries++) {
        uint32_t slot = (hint + tries) % g_sb.max_cards;
        uint32_t blk = g_sb.bitmap_start + (slot / 4096);
        uint32_t byte_off = (slot % 4096) / 8;
        uint8_t bit = 1u << (slot % 8);

        // Cache one bitmap block — scan 4096 slots per SD read
        if (blk != cached_blk) {
            if (!sd_read_block(blk, buf)) continue;
            cached_blk = blk;

            // Fast scan: find first zero byte in this block
            uint32_t base_slot = (slot / 4096) * 4096;
            for (uint32_t b = 0; b < 512; b++) {
                if (buf[b] == 0xFF) continue;
                for (uint8_t bi = 0; bi < 8; bi++) {
                    if (!(buf[b] & (1u << bi))) {
                        uint32_t free_slot = base_slot + b * 8 + bi;
                        if (free_slot < g_sb.max_cards) {
                            g_sb.next_free_hint = (free_slot + 1) % g_sb.max_cards;
                            return (int32_t)free_slot;
                        }
                    }
                }
            }
            // Entire block full — skip to next block
            tries += 4095 - (slot % 4096);
            continue;
        }

        if (!(buf[byte_off] & bit)) {
            g_sb.next_free_hint = (slot + 1) % g_sb.max_cards;
            return (int32_t)slot;
        }
    }
    return -1;
}

// 3-tier lookup: SRAM → Flash (XIP) → SD hash table
static int32_t find_slot_for_key(uint32_t key) {
    // Tier 1: SRAM (hot cache)
    int32_t pos = index_find(key);
    if (pos >= 0) return (int32_t)g_slots[pos];

    // Tier 2: Flash index (XIP, zero-copy binary search)
    int32_t fslot = fidx_lookup(key);
    if (fslot >= 0) {
        index_insert(key, (uint32_t)fslot);  // promote to SRAM
        return fslot;
    }

    // Tier 3: On-disk hash table (1-2 SD reads)
    int32_t hslot = ht_lookup(key);
    if (hslot >= 0) {
        index_insert(key, (uint32_t)hslot);  // promote to SRAM
        return hslot;
    }

    return -1;
}

// ============================================================
// Put — with slot allocation
// ============================================================

bool kvsd_put(uint32_t key, const uint8_t *value, uint16_t len) {
    if (!g_ready || len > KVSD_MAX_PAYLOAD) return false;
    if (len >= 2 && (value[0] != KVSD_MAGIC_LO || value[1] != KVSD_MAGIC_HI))
        return false;

    int32_t old_blk = find_slot_for_key(key);

    // Compress
    uint8_t comp[PACKED_DATA_MAX];
    uint16_t clen = hs_compress(value, len, comp, PACKED_DATA_MAX);
    if (clen == 0) return false;

    // Append to open block (allocates new block if needed)
    uint32_t new_blk = packed_append(key, comp, clen, len);
    if (new_blk == UINT32_MAX) return false;

    // Update hash table: key → block_number
    ht_insert(key, new_blk);

    bool is_new = (old_blk < 0);
    index_insert(key, new_blk);
    g_fidx_count = 0;

    // Remove from old block if COW update (different block)
    if (!is_new && (uint32_t)old_blk != new_blk) {
        packed_remove_card((uint32_t)old_blk, key);
    }
    if (is_new) g_sb.total_cards++;
    g_sb.dirty = 1;
    return true;
}

// ============================================================
// Get — decompress from packed block
// ============================================================

const uint8_t *kvsd_get(uint32_t key, uint16_t *len) {
    if (!g_ready) return NULL;
    int32_t blk = find_slot_for_key(key);
    if (blk < 0) return NULL;
    uint16_t raw_len = packed_read_card((uint32_t)blk, key, g_card_buf, KVSD_MAX_PAYLOAD);
    if (raw_len == 0) return NULL;
    if (g_card_buf[0] != KVSD_MAGIC_LO || g_card_buf[1] != KVSD_MAGIC_HI) return NULL;
    if (len) *len = raw_len;
    return g_card_buf;
}

bool kvsd_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    if (!g_ready) return false;
    int32_t blk = find_slot_for_key(key);
    if (blk < 0) return false;
    uint16_t raw_len = packed_read_card((uint32_t)blk, key, out, KVSD_MAX_PAYLOAD);
    if (raw_len == 0) return false;
    if (out[0] != KVSD_MAGIC_LO || out[1] != KVSD_MAGIC_HI) return false;
    if (*len < raw_len) return false;
    *len = raw_len;
    if (version) *version = (uint16_t)out[2] | ((uint16_t)out[3]<<8);
    return true;
}

// ============================================================
// Delete — remove entry from packed block, compact
// ============================================================

bool kvsd_delete(uint32_t key) {
    if (!g_ready) return false;
    int32_t blk = find_slot_for_key(key);
    if (blk < 0) return false;

    packed_remove_card((uint32_t)blk, key);
    ht_remove(key);
    if (index_remove(key)) { g_sb.total_cards--; g_sb.dirty = 1; }
    return true;
}

bool kvsd_exists(uint32_t key) {
    if (!g_ready) return false;
    return find_slot_for_key(key) >= 0;
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

    // Scan SRAM index (tier 1) — sorted, so early exit is safe
    for (uint32_t i = 0; i < g_index_count && count < max; i++) {
        if ((g_index[i] & mask) == masked) out_keys[count++] = g_index[i];
        else if (g_index[i] > (masked | ~mask)) break;
    }
    uint32_t sram_count = count;  // mark end of SRAM results

    // Scan flash index (tier 2) — sorted merge dedup O(n+m)
    // Use a merge pointer into SRAM results for duplicate detection.
    uint32_t sp = 0;  // scan pointer into out_keys[0..sram_count)
    for (uint32_t i = 0; i < g_fidx_count && count < max; i++) {
        const fidx_entry_t *e = fidx_xip(i);
        if (e->key == 0xFFFFFFFF) break;
        if ((e->key & mask) != masked) {
            if (e->key > (masked | ~mask)) break;
            continue;
        }
        // Advance SRAM pointer past keys < flash key (both sorted)
        while (sp < sram_count && out_keys[sp] < e->key) sp++;
        // Duplicate if SRAM has this exact key
        if (sp < sram_count && out_keys[sp] == e->key) continue;
        out_keys[count++] = e->key;
    }

    // Note: range queries only cover keys in SRAM + flash XIP tiers.
    // The on-disk hash table doesn't support prefix scans. For full coverage,
    // ensure hot packs are loaded into SRAM via recent access, or use bitmap
    // scan for exhaustive enumeration (admin/query paths).
    return count;
}

// ============================================================
// Flush — persist superblock + flash index tier
// Hash table is updated inline on put/delete, so no keylist write needed.
// ============================================================

bool kvsd_flush(void) {
    if (!g_ready) return false;

    g_sb.dirty = 0;
    sb_write();

    // Update flash index tier (tier 2) from SRAM hot cache
    fidx_write_all(g_index, g_slots, g_index_count);

    web_log("[kvsd] flush sb+fidx (%lu sram)\n", (unsigned long)g_index_count);
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

uint32_t kvsd_record_count(void) { return g_sb.total_cards; }

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
bool kvsd_dirty(void) { return g_ready && g_sb.dirty; }

uint32_t kvsd_ota_start_block(void) {
    if (!g_ready) return 0;
    // Backward compat: old superblocks may have ota_start=0
    if (g_sb.ota_start == 0 || g_sb.ota_blocks == 0) return 1; // default
    return g_sb.ota_start;
}
