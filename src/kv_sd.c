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
    if (computed != hdr->crc32) return;  // entry data corrupted — discard

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

    // Pre-compute CRC from SRAM arrays so we can write the header last.
    // The byte layout matches what will be written (little-endian uint32_t pairs).
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = keys[i], sl = slots[i];
        for (int b = 0; b < 4; b++) {
            crc ^= (uint8_t)(k  >> (b * 8));
            for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        for (int b = 0; b < 4; b++) {
            crc ^= (uint8_t)(sl >> (b * 8));
            for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    crc ^= 0xFFFFFFFFu;

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

    // Reserve last 256 blocks for SD ring buffer (UDP overflow WAL)
    uint32_t usable = total_blocks - SDRING_GUARD_BLOCKS;

    // Block 0: superblock, blocks 1-1200: OTA staging (600KB)
    uint32_t reserved = 1 + KVSD_OTA_BLOCKS;
    g_sb.ota_start = 1;
    g_sb.ota_blocks = KVSD_OTA_BLOCKS;

    uint32_t available = usable - reserved;
    uint32_t index_total = available * 15 / 100;
    uint32_t data_total = available - index_total;

    uint32_t max_cards = data_total / KVSD_CARD_BLOCKS;
    uint32_t bitmap_blks = (max_cards + 4095) / 4096;
    uint32_t keylist_blks = (max_cards * 4 + 511) / 512;
    if (keylist_blks > index_total / 2) keylist_blks = index_total / 2;
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

    web_log("[kvsd] fmt %lu blks, max %lu cards\n",
            (unsigned long)usable, (unsigned long)max_cards);

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

    // Validate and load the flash index tier (tier 2) before the SD keylist.
    // fidx_count_entries() checks the header magic, version, and CRC; if the
    // check fails the flash index is silently empty — the SRAM index is then
    // rebuilt from the SD keylist below.  This activates the 3-tier index on
    // every boot, not only after a flush.
    fidx_count_entries();

    if (sb_read()) {
        web_log("[kvsd] %lu cards, max %lu\n",
                (unsigned long)g_sb.total_cards, (unsigned long)g_sb.max_cards);

        uint32_t saved = g_sb.total_cards;
        if (saved > 0 && saved <= KVSD_INDEX_MAX && g_sb.keylist_blocks > 0) {
            uint32_t loaded = 0;
            for (uint32_t b = 0; b < g_sb.keylist_blocks && loaded < saved; b++) {
                uint8_t buf[512];
                if (!sd_read_block(g_sb.keylist_start + b, buf)) break;
                for (uint32_t k = 0; k < 64 && loaded < saved; k++) {
                    uint32_t off = k * 8;
                    uint32_t key = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
                                   ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
                    uint32_t slot = (uint32_t)buf[off+4] | ((uint32_t)buf[off+5]<<8) |
                                    ((uint32_t)buf[off+6]<<16) | ((uint32_t)buf[off+7]<<24);
                    if (key == 0xFFFFFFFF) break;
                    g_index[loaded] = key;
                    g_slots[loaded] = slot;
                    loaded++;
                }
            }
            g_index_count = loaded;
        }

        if (g_index_count == 0 && g_sb.total_cards > 0) {
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
                        uint32_t slot = base + by*8 + bi;
                        uint8_t hdr[512];
                        if (!sd_read_block(slot_to_block(slot), hdr)) continue;
                        uint32_t key = hdr[508] | ((uint32_t)hdr[509]<<8) |
                                       ((uint32_t)hdr[510]<<16) | ((uint32_t)hdr[511]<<24);
                        if (key == 0 || key == 0xFFFFFFFF) continue;
                        g_index[g_index_count] = key;
                        g_slots[g_index_count] = slot;
                        g_index_count++;
                    }
                }
            }
            // Sort by key
            for (uint32_t i = 1; i < g_index_count; i++) {
                uint32_t k = g_index[i], s = g_slots[i];
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && g_index[j] > k) {
                    g_index[j+1] = g_index[j]; g_slots[j+1] = g_slots[j]; j--;
                }
                g_index[j+1] = k; g_slots[j+1] = s;
            }
            web_log("[kvsd] scan: %lu keys\n", (unsigned long)g_index_count);
        }
    } else {
        web_log("[kvsd] no superblock, formatting\n");
        format_sd(info.block_count);
    }

    g_ready = true;
    web_log("[kvsd] ready %lu/%lu\n",
            (unsigned long)g_index_count, (unsigned long)KVSD_INDEX_MAX);
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

// 3-tier lookup: SRAM → Flash (XIP) → not found
// SD keylist is only used at boot to populate SRAM+flash
static int32_t find_slot_for_key(uint32_t key) {
    // Tier 1: SRAM (hot cache)
    int32_t pos = index_find(key);
    if (pos >= 0) return (int32_t)g_slots[pos];

    // Tier 2: Flash index (XIP, zero-copy binary search)
    int32_t fslot = fidx_lookup(key);
    if (fslot >= 0) {
        // Promote to SRAM cache
        index_insert(key, (uint32_t)fslot);
        return fslot;
    }

    return -1;
}

// ============================================================
// Put — with slot allocation
// ============================================================

bool kvsd_put(uint32_t key, const uint8_t *value, uint16_t len) {
    if (!g_ready || len > KVSD_CARD_SIZE - 4) return false;

    int32_t old_slot = find_slot_for_key(key);

    // COW: always allocate a new slot — never overwrite in-place
    uint32_t new_slot;
    {
        uint32_t card_id = key & 0x3FFFFF;
        if (old_slot < 0 && card_id < g_sb.max_cards && !bitmap_get(card_id)) {
            new_slot = card_id;
        } else {
            int32_t s = alloc_slot();
            if (s < 0) return false;
            new_slot = (uint32_t)s;
        }
    }

    uint8_t card[KVSD_CARD_SIZE];
    memset(card, 0, KVSD_CARD_SIZE);
    memcpy(card, value, len);
    if (card[0] != KVSD_MAGIC_LO || card[1] != KVSD_MAGIC_HI) return false;
    card[508] = (uint8_t)(key);
    card[509] = (uint8_t)(key >> 8);
    card[510] = (uint8_t)(key >> 16);
    card[511] = (uint8_t)(key >> 24);

    if (!write_card(new_slot, card)) return false;

    bitmap_set(new_slot, true);
    bool is_new = (old_slot < 0);
    index_insert(key, new_slot);

    if (!is_new && (uint32_t)old_slot != new_slot) {
        bitmap_set((uint32_t)old_slot, false);
    }
    if (is_new) g_sb.total_cards++;
    g_sb.dirty = 1;
    return true;
}

// ============================================================
// Get
// ============================================================

const uint8_t *kvsd_get(uint32_t key, uint16_t *len) {
    if (!g_ready) return NULL;
    int32_t slot = find_slot_for_key(key);
    if (slot < 0) return NULL;
    if (!sd_read_blocks(slot_to_block((uint32_t)slot), g_card_buf, KVSD_CARD_BLOCKS)) return NULL;
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
    if (!sd_read_blocks(slot_to_block((uint32_t)slot), card, KVSD_CARD_BLOCKS)) return false;
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
    return find_slot_for_key(key) >= 0;  // checks SRAM + flash index
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

    // Scan SRAM index (tier 1)
    for (uint32_t i = 0; i < g_index_count && count < max; i++) {
        if ((g_index[i] & mask) == masked) out_keys[count++] = g_index[i];
        else if (g_index[i] > (masked | ~mask)) break;
    }

    // Scan flash index (tier 2) for entries not in SRAM
    for (uint32_t i = 0; i < g_fidx_count && count < max; i++) {
        const fidx_entry_t *e = fidx_xip(i);
        if (e->key == 0xFFFFFFFF) break;
        if ((e->key & mask) != masked) {
            if (e->key > (masked | ~mask)) break;
            continue;
        }
        // Skip if already in SRAM results (dedup)
        bool dup = false;
        for (uint32_t j = 0; j < count; j++) {
            if (out_keys[j] == e->key) { dup = true; break; }
        }
        if (!dup) out_keys[count++] = e->key;
    }

    return count;
}

// ============================================================
// Flush — persist index to SD
// ============================================================

bool kvsd_flush(void) {
    if (!g_ready) return false;

    // Write sorted key+slot pairs (64 per block)
    uint32_t to_save = g_index_count;
    uint32_t blocks_needed = (to_save + 63) / 64;
    if (blocks_needed > g_sb.keylist_blocks) blocks_needed = g_sb.keylist_blocks;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint8_t buf[512];
        memset(buf, 0xFF, 512);
        uint32_t start = b * 64;
        for (uint32_t k = 0; k < 64 && start+k < to_save; k++) {
            uint32_t off = k * 8;
            uint32_t key = g_index[start+k];
            uint32_t slot = g_slots[start+k];
            buf[off]=(uint8_t)key; buf[off+1]=(uint8_t)(key>>8);
            buf[off+2]=(uint8_t)(key>>16); buf[off+3]=(uint8_t)(key>>24);
            buf[off+4]=(uint8_t)slot; buf[off+5]=(uint8_t)(slot>>8);
            buf[off+6]=(uint8_t)(slot>>16); buf[off+7]=(uint8_t)(slot>>24);
        }
        if (!sd_write_block(g_sb.keylist_start + b, buf)) return false;
    }

    if (blocks_needed < g_sb.keylist_blocks) {
        uint8_t ff[512]; memset(ff, 0xFF, 512);
        sd_write_block(g_sb.keylist_start + blocks_needed, ff);
    }

    g_sb.dirty = 0;
    sb_write();

    // Also update flash index tier (tier 2) from SRAM
    fidx_write_all(g_index, g_slots, g_index_count);

    web_log("[kvsd] flush %lu\n", (unsigned long)to_save);
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

uint32_t kvsd_ota_start_block(void) {
    if (!g_ready) return 0;
    // Backward compat: old superblocks may have ota_start=0
    if (g_sb.ota_start == 0 || g_sb.ota_blocks == 0) return 1; // default
    return g_sb.ota_start;
}
