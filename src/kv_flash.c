#include "kv_flash.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

#define XIP_BASE_ADDR 0x10000000u

// V1 page magic (legacy, no longer written; skipped on recovery)
#define KV_PAGE_MAGIC        0x4B565031u // KVP1
// V2 page magic: includes sequence + header CRC for deterministic recovery
#define KV_PAGE_MAGIC_V2     0x4B565032u // KVP2
#define KV_PAGE_VERSION_1    0x01u

// Mutation-group commit marker magic, stored at the same offset as hdr.magic
// in the unified record layout so the scanner can dispatch on it.
#define KV_COMMIT_MAGIC      0x4B564D43u // KVMC

// Page lifecycle states stored in kv_page_hdr_t.page_state.
// A page transitions: NEW (just written) → ACTIVE (index references it)
// → STALE (superseded by CoW write) → RECLAIMABLE (no live references,
// safe to erase).  The states are informational; correctness relies on the
// in-memory live-reference check in page_has_live_refs().
#define KV_PAGE_STATE_NEW          0x01u
#define KV_PAGE_STATE_ACTIVE       0x02u
#define KV_PAGE_STATE_STALE        0x03u
#define KV_PAGE_STATE_RECLAIMABLE  0x04u

// Byte offset of hdr_crc within kv_page_hdr_t — CRC covers everything before
// this field (magic + version + page_state + _pad + sequence = 12 bytes).
#define KV_PAGE_HDR_CRC_OFFSET  12u

// Aligned size of the commit record (14 raw bytes, padded to 4-byte boundary).
#define KV_COMMIT_ALIGNED    16u

#define KV_REC_FLAG_COMP     0x01u
#define KV_REC_FLAG_TOMB     0x02u

#define KV_LOC_PAGE_BITS     12u
#define KV_LOC_OFF_BITS      10u
#define KV_LOC_LEN_BITS      10u

#define KV_LOC_OFF_SHIFT     KV_LOC_LEN_BITS
#define KV_LOC_PAGE_SHIFT    (KV_LOC_OFF_BITS + KV_LOC_LEN_BITS)
#define KV_LOC_LEN_MASK      ((1u << KV_LOC_LEN_BITS) - 1u)
#define KV_LOC_OFF_MASK      ((1u << KV_LOC_OFF_BITS) - 1u)
#define KV_LOC_PAGE_MASK     ((1u << KV_LOC_PAGE_BITS) - 1u)

#define KV_LEN_QUANTUM       4u // packed length/offset units

#define KV_INDEX_CAPACITY    512u   // flash KV: packs 0-1 only (schemas + users)

// Dead log (append-only pages)
#define KV_DEADLOG_MAGIC     0x444C4731u // DLG1

// Page header: written once when a sector is first prepared.
// hdr_crc covers the 12 bytes before it (magic+version+page_state+_pad+sequence)
// so a partial page-header write is detectable on recovery.
typedef struct __attribute__((packed)) {
    uint32_t magic;       // KV_PAGE_MAGIC_V2
    uint8_t  version;     // KV_PAGE_VERSION_1
    uint8_t  page_state;  // KV_PAGE_STATE_* lifecycle marker
    uint16_t _pad;
    uint32_t sequence;    // monotonically increasing per page allocation; used
                          // on recovery to establish write ordering across pages
    uint32_t hdr_crc;     // CRC32 of the 12 bytes above; validates header integrity
} kv_page_hdr_t;

_Static_assert(sizeof(kv_page_hdr_t) == 16, "kv_page_hdr_t size");

// Record prefix: written before each value payload.
// mutation_group ties this record to its commit marker below, ensuring that
// partial writes (power loss after record but before commit) are discardable.
typedef struct __attribute__((packed)) {
    uint16_t rec_len;         // total aligned record size (prefix + payload)
    uint32_t mutation_group;  // links this record to its kv_commit_rec_t
    kv_header_t hdr;          // per-record magic / key / version / CRC
} kv_rec_prefix_t;

_Static_assert(sizeof(kv_rec_prefix_t) == 26, "kv_rec_prefix_t size");

// Commit marker: written immediately after the data record in the same
// flash-programming buffer.  commit_magic sits at byte offset 6, the same
// offset as hdr.magic in kv_rec_prefix_t, so the recovery scanner can
// dispatch on a single magic read.
// A mutation group is valid only when this marker (with matching group ID
// and valid CRC) follows the data record.  Records without a commit marker
// are discarded on recovery — they represent a partial write interrupted by
// power loss.
typedef struct __attribute__((packed)) {
    uint16_t rec_len;         // KV_COMMIT_ALIGNED (16)
    uint32_t mutation_group;  // must match the preceding data record's group
    uint32_t commit_magic;    // KV_COMMIT_MAGIC — distinguishes from data records
    uint32_t crc;             // CRC32 of (mutation_group || commit_magic) — 8 bytes
} kv_commit_rec_t;

_Static_assert(sizeof(kv_commit_rec_t) == 14, "kv_commit_rec_t size");

typedef struct __attribute__((packed)) {
    uint32_t magic;
} deadlog_hdr_t;

_Static_assert(sizeof(deadlog_hdr_t) == 4, "deadlog_hdr_t size");

typedef struct __attribute__((packed)) {
    uint16_t page_idx;
    uint16_t reserved;
} deadlog_entry_t;

_Static_assert(sizeof(deadlog_entry_t) == 4, "deadlog_entry_t size");

static uint32_t g_keys[KV_INDEX_CAPACITY];
static uint32_t g_locs[KV_INDEX_CAPACITY];
static uint16_t g_versions[KV_INDEX_CAPACITY];
static uint8_t g_flags[KV_INDEX_CAPACITY];
static uint32_t g_count = 0;

// Page allocation sequence: incremented each time a new page is prepared.
// Loaded from the highest valid page header on recovery so that new pages
// always get a strictly higher sequence number.
static uint32_t g_page_sequence = 0;

// Mutation group counter: incremented for each kv_put / kv_delete call.
// Each data record and its commit marker share the same group ID, enabling
// the recovery scanner to detect and discard uncommitted partial writes.
static uint32_t g_mutation_group = 0;

static uint16_t g_write_page = 0;
static uint16_t g_write_off = sizeof(kv_page_hdr_t);

static uint16_t g_deadlog_write_page = 0;
static uint16_t g_deadlog_write_off = sizeof(deadlog_hdr_t);
static uint16_t g_deadlog_read_page = 0;
static uint16_t g_deadlog_read_off = sizeof(deadlog_hdr_t);

static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static const uint8_t *xip_ptr(uint32_t flash_off) {
    return (const uint8_t *)(XIP_BASE_ADDR + flash_off);
}

#define KV_LZ_WINDOW      512u
#define KV_LZ_MIN_MATCH   3u
#define KV_LZ_MAX_MATCH   18u
#define KV_COMPRESS_MIN   256u

static bool encode_lz_lite(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len) {
    uint16_t ip = 0;
    uint16_t op = 0;
    uint16_t lit_start = 0;
    uint16_t lit_len = 0;

    while (ip < in_len) {
        uint16_t best_len = 0;
        uint16_t best_off = 0;
        uint16_t search = (ip < KV_LZ_WINDOW) ? ip : KV_LZ_WINDOW;

        for (uint16_t off = 1; off <= search; off++) {
            uint16_t match_len = 0;
            while (match_len < KV_LZ_MAX_MATCH &&
                   (uint32_t)ip + match_len < in_len &&
                   in[ip - off + match_len] == in[ip + match_len]) {
                match_len++;
            }
            if (match_len >= KV_LZ_MIN_MATCH && match_len > best_len) {
                best_len = match_len;
                best_off = off;
                if (best_len == KV_LZ_MAX_MATCH) break;
            }
        }

        if (best_len >= KV_LZ_MIN_MATCH) {
            if (lit_len > 0) {
                if ((uint32_t)op + 1u + lit_len > *out_len) return false;
                out[op++] = (uint8_t)(lit_len - 1u);
                memcpy(out + op, in + lit_start, lit_len);
                op = (uint16_t)(op + lit_len);
                lit_len = 0;
            }

            if ((uint32_t)op + 2u > *out_len) return false;
            uint16_t offm1 = (uint16_t)(best_off - 1u);
            out[op++] = (uint8_t)(0x80u | ((uint8_t)(best_len - KV_LZ_MIN_MATCH) << 4) | ((offm1 >> 8) & 0x0Fu));
            out[op++] = (uint8_t)(offm1 & 0xFFu);
            ip = (uint16_t)(ip + best_len);
            lit_start = ip;
            continue;
        }

        if (lit_len == 0) lit_start = ip;
        lit_len++;
        ip++;

        if (lit_len == 128u) {
            if ((uint32_t)op + 1u + lit_len > *out_len) return false;
            out[op++] = (uint8_t)(lit_len - 1u);
            memcpy(out + op, in + lit_start, lit_len);
            op = (uint16_t)(op + lit_len);
            lit_len = 0;
        }
    }

    if (lit_len > 0) {
        if ((uint32_t)op + 1u + lit_len > *out_len) return false;
        out[op++] = (uint8_t)(lit_len - 1u);
        memcpy(out + op, in + lit_start, lit_len);
        op = (uint16_t)(op + lit_len);
    }

    *out_len = op;
    return true;
}

static bool decode_lz_lite(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t out_len) {
    uint16_t ip = 0;
    uint16_t op = 0;

    while (ip < in_len) {
        uint8_t tag = in[ip++];
        if ((tag & 0x80u) == 0) {
            uint16_t lit_len = (uint16_t)tag + 1u;
            if ((uint32_t)ip + lit_len > in_len) return false;
            if ((uint32_t)op + lit_len > out_len) return false;
            memcpy(out + op, in + ip, lit_len);
            ip = (uint16_t)(ip + lit_len);
            op = (uint16_t)(op + lit_len);
            continue;
        }

        if (ip >= in_len) return false;
        uint16_t match_len = (uint16_t)(((tag >> 4) & 0x07u) + KV_LZ_MIN_MATCH);
        uint16_t off = (uint16_t)((((uint16_t)tag & 0x0Fu) << 8) | in[ip++]);
        off = (uint16_t)(off + 1u);
        if (off == 0 || off > op) return false;
        if ((uint32_t)op + match_len > out_len) return false;

        for (uint16_t i = 0; i < match_len; i++) {
            out[op] = out[op - off];
            op++;
        }
    }

    return op == out_len;
}

static uint32_t pack_loc(uint16_t page, uint16_t off_bytes, uint16_t len_bytes) {
    uint16_t off_q = (uint16_t)(off_bytes / KV_LEN_QUANTUM);
    uint16_t len_q = (uint16_t)((len_bytes + (KV_LEN_QUANTUM - 1u)) / KV_LEN_QUANTUM);
    if (off_q > KV_LOC_OFF_MASK) off_q = KV_LOC_OFF_MASK;
    if (len_q > KV_LOC_LEN_MASK) len_q = KV_LOC_LEN_MASK;
    return (((uint32_t)page & KV_LOC_PAGE_MASK) << KV_LOC_PAGE_SHIFT)
         | (((uint32_t)off_q & KV_LOC_OFF_MASK) << KV_LOC_OFF_SHIFT)
         | ((uint32_t)len_q & KV_LOC_LEN_MASK);
}

static void unpack_loc(uint32_t loc, uint16_t *page, uint16_t *off_bytes, uint16_t *len_bytes) {
    uint16_t p = (uint16_t)((loc >> KV_LOC_PAGE_SHIFT) & KV_LOC_PAGE_MASK);
    uint16_t o = (uint16_t)((loc >> KV_LOC_OFF_SHIFT) & KV_LOC_OFF_MASK);
    uint16_t l = (uint16_t)(loc & KV_LOC_LEN_MASK);
    if (page) *page = p;
    if (off_bytes) *off_bytes = (uint16_t)(o * KV_LEN_QUANTUM);
    if (len_bytes) *len_bytes = (uint16_t)(l * KV_LEN_QUANTUM);
}

static int idx_find_linear(uint32_t key) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_keys[i] == key) return (int)i;
        if (g_keys[i] > key) return -(int)(i + 1);
    }
    return -(int)(g_count + 1u);
}

static bool idx_set(uint32_t key, uint32_t loc, uint16_t version, uint8_t flags) {
    int f = idx_find_linear(key);
    if (f >= 0) {
        g_locs[(uint32_t)f] = loc;
        g_versions[(uint32_t)f] = version;
        g_flags[(uint32_t)f] = flags;
        return true;
    }
    if (g_count >= KV_INDEX_CAPACITY) return false;
    uint32_t pos = (uint32_t)(-f - 1);
    if (pos < g_count) {
        memmove(&g_keys[pos + 1], &g_keys[pos], (g_count - pos) * sizeof(uint32_t));
        memmove(&g_locs[pos + 1], &g_locs[pos], (g_count - pos) * sizeof(uint32_t));
        memmove(&g_versions[pos + 1], &g_versions[pos], (g_count - pos) * sizeof(uint16_t));
        memmove(&g_flags[pos + 1], &g_flags[pos], (g_count - pos) * sizeof(uint8_t));
    }
    g_keys[pos] = key;
    g_locs[pos] = loc;
    g_versions[pos] = version;
    g_flags[pos] = flags;
    g_count++;
    return true;
}

static void idx_remove(uint32_t key) {
    int f = idx_find_linear(key);
    if (f < 0) return;
    uint32_t i = (uint32_t)f;
    if (i + 1u < g_count) {
        memmove(&g_keys[i], &g_keys[i + 1], (g_count - i - 1u) * sizeof(uint32_t));
        memmove(&g_locs[i], &g_locs[i + 1], (g_count - i - 1u) * sizeof(uint32_t));
        memmove(&g_versions[i], &g_versions[i + 1], (g_count - i - 1u) * sizeof(uint16_t));
        memmove(&g_flags[i], &g_flags[i + 1], (g_count - i - 1u) * sizeof(uint8_t));
    }
    g_count--;
}

static bool idx_get(uint32_t key, uint32_t *loc, uint16_t *version, uint8_t *flags) {
    int f = idx_find_linear(key);
    if (f < 0) return false;
    uint32_t i = (uint32_t)f;
    if (loc) *loc = g_locs[i];
    if (version) *version = g_versions[i];
    if (flags) *flags = g_flags[i];
    return true;
}

// ensure_page_ready: prepare a sector for appending.
// If the sector already has a valid V2 header (magic + version + CRC), return
// immediately — no re-write needed.  Otherwise erase (if non-blank) and write
// a fresh V2 header with an incremented sequence number.
// Invariant: after this call the sector's header CRC is valid and sequence
// strictly exceeds all previously seen sequences.
static bool ensure_page_ready(uint16_t page) {
    uint32_t page_off = KV_REGION_START + (uint32_t)page * KV_SECTOR_SIZE;
    const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
    if (ph->magic == KV_PAGE_MAGIC_V2 && ph->version == KV_PAGE_VERSION_1) {
        uint32_t expected = crc32((const uint8_t *)ph, KV_PAGE_HDR_CRC_OFFSET);
        if (expected == ph->hdr_crc) return true;
    }

    uint8_t hdrbuf[KV_PAGE_SIZE];
    memset(hdrbuf, 0xFF, sizeof(hdrbuf));
    kv_page_hdr_t *nh = (kv_page_hdr_t *)hdrbuf;
    nh->magic      = KV_PAGE_MAGIC_V2;
    nh->version    = KV_PAGE_VERSION_1;
    nh->page_state = KV_PAGE_STATE_NEW;  // lifecycle: freshly written
    nh->_pad       = 0;
    nh->sequence   = ++g_page_sequence;  // strictly increasing — recovery picks highest
    nh->hdr_crc    = crc32((const uint8_t *)nh, KV_PAGE_HDR_CRC_OFFSET);

    uint32_t ints = save_and_disable_interrupts();
    if (ph->magic != KV_FREE) {
        flash_range_erase(page_off, KV_SECTOR_SIZE);
    }
    flash_range_program(page_off, hdrbuf, KV_PAGE_SIZE);
    restore_interrupts(ints);
    return true;
}

static bool page_is_prepared_empty(uint16_t page) {
    uint32_t page_off = KV_REGION_START + (uint32_t)page * KV_SECTOR_SIZE;
    const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
    if (ph->magic != KV_PAGE_MAGIC_V2) return false;
    const uint32_t *probe = (const uint32_t *)xip_ptr(page_off + sizeof(kv_page_hdr_t));
    return *probe == 0xFFFFFFFFu;
}

static bool find_next_append_page(uint16_t start_page, uint16_t *out_page) {
    for (uint32_t i = 1; i <= KV_SECTOR_COUNT; i++) {
        uint16_t cand = (uint16_t)((start_page + i) % KV_SECTOR_COUNT);
        uint32_t page_off = KV_REGION_START + (uint32_t)cand * KV_SECTOR_SIZE;
        const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
        if (ph->magic == KV_FREE || ph->magic != KV_PAGE_MAGIC_V2 || page_is_prepared_empty(cand)) {
            *out_page = cand;
            return true;
        }
    }
    return false;
}

static void prewarm_next_append_page(void) {
    uint16_t next_page = 0;
    if (!find_next_append_page(g_write_page, &next_page)) return;
    if (next_page == g_write_page) return;

    uint32_t page_off = KV_REGION_START + (uint32_t)next_page * KV_SECTOR_SIZE;
    const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
    if (ph->magic == KV_FREE) {
        (void)ensure_page_ready(next_page);
    }
}

static bool select_append_page(uint16_t need) {
    if ((uint32_t)g_write_off + need <= KV_SECTOR_SIZE) {
        uint32_t page_off = KV_REGION_START + (uint32_t)g_write_page * KV_SECTOR_SIZE;
        const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
        if (ph->magic == KV_PAGE_MAGIC_V2 || ph->magic == KV_FREE) return true;
    }

    uint16_t cand = 0;
    if (!find_next_append_page(g_write_page, &cand)) return false;
    g_write_page = cand;
    g_write_off = sizeof(kv_page_hdr_t);
    return true;
}

static void deadlog_reset_page(uint16_t p) {
    uint32_t off = KV_DEADLOG_START + (uint32_t)p * KV_SECTOR_SIZE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, KV_SECTOR_SIZE);
    restore_interrupts(ints);
}

static bool deadlog_init_page_if_needed(uint16_t p) {
    uint32_t off = KV_DEADLOG_START + (uint32_t)p * KV_SECTOR_SIZE;
    const deadlog_hdr_t *h = (const deadlog_hdr_t *)xip_ptr(off);
    if (h->magic == KV_DEADLOG_MAGIC) return true;
    if (h->magic != KV_FREE) return false;

    uint8_t buf[KV_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    ((deadlog_hdr_t *)buf)->magic = KV_DEADLOG_MAGIC;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, buf, KV_PAGE_SIZE);
    restore_interrupts(ints);
    return true;
}

static bool deadlog_append(uint16_t page_idx) {
    for (uint32_t n = 0; n < KV_DEADLOG_SECTORS; n++) {
        uint16_t p = (uint16_t)((g_deadlog_write_page + n) % KV_DEADLOG_SECTORS);
        if (!deadlog_init_page_if_needed(p)) continue;
        uint32_t base = KV_DEADLOG_START + (uint32_t)p * KV_SECTOR_SIZE;

        uint16_t off = (p == g_deadlog_write_page) ? g_deadlog_write_off : sizeof(deadlog_hdr_t);
        while ((uint32_t)off + sizeof(deadlog_entry_t) <= KV_SECTOR_SIZE) {
            const uint16_t *probe = (const uint16_t *)xip_ptr(base + off);
            if (*probe == 0xFFFFu) break;
            off = (uint16_t)(off + sizeof(deadlog_entry_t));
        }
        if ((uint32_t)off + sizeof(deadlog_entry_t) <= KV_SECTOR_SIZE) {
            deadlog_entry_t e = {.page_idx = page_idx, .reserved = 0};
            uint32_t ints = save_and_disable_interrupts();
            flash_range_program(base + off, (const uint8_t *)&e, sizeof(e));
            restore_interrupts(ints);
            g_deadlog_write_page = p;
            g_deadlog_write_off = (uint16_t)(off + sizeof(deadlog_entry_t));
            return true;
        }
    }
    deadlog_reset_page(g_deadlog_write_page);
    g_deadlog_write_off = sizeof(deadlog_hdr_t);
    if (!deadlog_init_page_if_needed(g_deadlog_write_page)) return false;
    deadlog_entry_t e = {.page_idx = page_idx, .reserved = 0};
    uint32_t base = KV_DEADLOG_START + (uint32_t)g_deadlog_write_page * KV_SECTOR_SIZE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(base + g_deadlog_write_off, (const uint8_t *)&e, sizeof(e));
    restore_interrupts(ints);
    g_deadlog_write_off = (uint16_t)(g_deadlog_write_off + sizeof(deadlog_entry_t));
    return true;
}

static bool page_has_live_refs(uint16_t page_idx) {
    for (uint32_t i = 0; i < g_count; i++) {
        uint16_t p = 0;
        unpack_loc(g_locs[i], &p, NULL, NULL);
        if (p == page_idx) return true;
    }
    return false;
}

// append_record: write a data record followed immediately by its commit marker.
//
// Write ordering invariant (issue requirement §4):
//   1. New data record (CoW — never overwrites existing page data)
//   2. Commit marker with matching mutation_group
// Both are packed into the same programming buffer so that they land in flash
// within a single (or two adjacent) flash_range_program calls.  A commit
// marker that cannot fit on the current page is included in the progbuf and
// the page selection ensures there is room for the combined total.
//
// Recovery (kv_init) only indexes records whose mutation_group has a valid
// commit marker, so a power-loss between steps 1 and 2 leaves the record
// effectively invisible on the next boot — it is simply skipped.
static bool append_record(uint32_t key, const uint8_t *raw, uint16_t raw_len, uint16_t version,
                          uint8_t flags, uint32_t mutation_group, uint32_t *out_loc) {
    uint8_t enc[KV_MAX_VALUE + 64];
    const uint8_t *stored = raw;
    uint16_t stored_len = raw_len;
    uint8_t rec_flags = flags;

    uint16_t enc_cap = sizeof(enc);
    if (raw_len >= KV_COMPRESS_MIN &&
        encode_lz_lite(raw, raw_len, enc, &enc_cap) &&
        enc_cap < raw_len) {
        stored = enc;
        stored_len = enc_cap;
        rec_flags |= KV_REC_FLAG_COMP;
    }

    kv_rec_prefix_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.mutation_group   = mutation_group;
    rp.hdr.magic        = KV_MAGIC;
    rp.hdr.key          = key;
    rp.hdr.raw_len      = raw_len;
    rp.hdr.store_len    = stored_len;
    rp.hdr.version      = version;
    rp.hdr.flags        = rec_flags;
    rp.hdr.checksum     = crc32(stored, stored_len);
    rp.rec_len          = (uint16_t)(sizeof(kv_rec_prefix_t) + stored_len);
    uint16_t rec_len_aligned = (uint16_t)((rp.rec_len + 3u) & ~3u);

    // Reserve space for both the data record AND its commit marker so they
    // always land on the same page.  This guarantees recovery can match them
    // without cross-page look-ahead.
    if (!select_append_page((uint16_t)(rec_len_aligned + KV_COMMIT_ALIGNED))) return false;
    if (!ensure_page_ready(g_write_page)) return false;

    uint32_t page_off = KV_REGION_START + (uint32_t)g_write_page * KV_SECTOR_SIZE;
    uint32_t rec_off  = page_off + g_write_off;

    // Build the programming buffer: [record][commit_marker][0xFF padding].
    // The commit marker sits at byte rec_len_aligned within buf.
    static uint8_t buf[KV_MAX_VALUE + 128];
    uint16_t total = (uint16_t)(rec_len_aligned + KV_COMMIT_ALIGNED);
    memset(buf, 0xFF, total);
    memcpy(buf, &rp, sizeof(rp));
    if (stored_len > 0) memcpy(buf + sizeof(rp), stored, stored_len);

    // Embed commit marker directly after the aligned record.
    kv_commit_rec_t *cm = (kv_commit_rec_t *)(buf + rec_len_aligned);
    cm->rec_len        = KV_COMMIT_ALIGNED;
    cm->mutation_group = mutation_group;
    cm->commit_magic   = KV_COMMIT_MAGIC;
    // CRC covers the 8 bytes: mutation_group || commit_magic.
    // This detects bit-flip corruption in the commit marker itself.
    cm->crc = crc32((const uint8_t *)&cm->mutation_group, 8u);

    uint16_t prog_len = (uint16_t)((total + (KV_PAGE_SIZE - 1u)) & ~(KV_PAGE_SIZE - 1u));
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(rec_off, buf, prog_len);
    restore_interrupts(ints);

    if (out_loc) *out_loc = pack_loc(g_write_page, g_write_off, rec_len_aligned);
    g_write_off = (uint16_t)(g_write_off + total);
    prewarm_next_append_page();
    return true;
}

static bool read_record(uint32_t loc, uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    uint16_t page = 0, off = 0;
    unpack_loc(loc, &page, &off, NULL);
    if (page >= KV_SECTOR_COUNT) return false;

    uint32_t rec_flash_off = KV_REGION_START + (uint32_t)page * KV_SECTOR_SIZE + off;
    const kv_rec_prefix_t *rp = (const kv_rec_prefix_t *)xip_ptr(rec_flash_off);

    if (rp->hdr.magic != KV_MAGIC || rp->hdr.key != key) return false;
    if (rp->hdr.flags & KV_REC_FLAG_TOMB) return false;
    if (rp->hdr.raw_len > KV_MAX_VALUE || rp->hdr.store_len > KV_MAX_VALUE) return false;
    if (rp->rec_len < sizeof(kv_rec_prefix_t)) return false;
    if ((uint32_t)off + rp->rec_len > KV_SECTOR_SIZE) return false;

    const uint8_t *stored = xip_ptr(rec_flash_off + sizeof(kv_rec_prefix_t));
    if (crc32(stored, rp->hdr.store_len) != rp->hdr.checksum) return false;

    if (len && *len < rp->hdr.raw_len) return false;
    if (out && len) {
        if (rp->hdr.flags & KV_REC_FLAG_COMP) {
            if (!decode_lz_lite(stored, rp->hdr.store_len, out, rp->hdr.raw_len)) return false;
        } else if (rp->hdr.raw_len > 0) {
            memcpy(out, stored, rp->hdr.raw_len);
        }
        *len = rp->hdr.raw_len;
    } else if (len) {
        *len = rp->hdr.raw_len;
    }
    if (version) *version = rp->hdr.version;
    return true;
}

void kv_wipe(void) {
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t off = KV_REGION_START; off < KV_DEADLOG_END; off += KV_SECTOR_SIZE) {
        flash_range_erase(off, KV_SECTOR_SIZE);
    }
    restore_interrupts(irq);
    g_count = 0;
    g_write_page = 0;
    g_write_off = sizeof(kv_page_hdr_t);
    g_page_sequence   = 0;
    g_mutation_group  = 0;
}

// kv_init: deterministic recovery routine.
//
// Recovery algorithm (issue requirement §5):
//   1. Scan all sectors.  Accept only V2 pages (KV_PAGE_MAGIC_V2 + version +
//      valid header CRC).  Skip V1/legacy pages — they require an admin wipe.
//   2. Track the highest valid page sequence seen; use it to seed g_page_sequence
//      so that new pages always receive a strictly higher number.
//   3. Within each valid page, scan records sequentially using a state-machine:
//      - data record   → hold as "pending"; discard any previous pending
//      - commit record → if CRC valid and mutation_group matches pending,
//                        apply the pending record to the SRAM index;
//                        otherwise discard (uncommitted partial write)
//      - end sentinel  → exit inner loop
//      - unknown magic → exit inner loop (corrupt tail)
//   4. Any pending record at page-end has no commit marker (power-loss window)
//      and is silently discarded — this makes recovery idempotent.
//   5. Version-based conflict resolution: among multiple committed records for
//      the same key (from different pages), keep the highest version.
void kv_init(void) {
    g_count          = 0;
    g_write_page     = 0;
    g_write_off      = sizeof(kv_page_hdr_t);
    g_page_sequence  = 0;
    g_mutation_group = 0;

    uint16_t last_page = 0;
    uint16_t last_off  = sizeof(kv_page_hdr_t);
    bool saw_data = false;

    for (uint16_t p = 0; p < KV_SECTOR_COUNT; p++) {
        uint32_t page_off = KV_REGION_START + (uint32_t)p * KV_SECTOR_SIZE;
        const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);

        // Require V2 format: magic, version, and valid header CRC.
        // V1 (KV_PAGE_MAGIC) pages are deliberately skipped — any existing V1
        // data requires an admin wipe to migrate to the V2 format.
        if (ph->magic != KV_PAGE_MAGIC_V2 || ph->version != KV_PAGE_VERSION_1) continue;
        uint32_t expected_hdr_crc = crc32((const uint8_t *)ph, KV_PAGE_HDR_CRC_OFFSET);
        if (ph->hdr_crc != expected_hdr_crc) continue;  // header bit-flip — skip page

        // Keep g_page_sequence ahead of all seen sequences so that the next
        // ensure_page_ready() call produces a strictly higher sequence number.
        if (ph->sequence > g_page_sequence) g_page_sequence = ph->sequence;

        uint16_t off = sizeof(kv_page_hdr_t);

        // Pending uncommitted record for this page scan.  Each kv_put/kv_delete
        // writes exactly one data record followed by one commit marker, so at
        // most one record can be pending at any time within a single scan.
        bool     has_pending      = false;
        uint32_t pending_key      = 0;
        uint32_t pending_loc      = 0;
        uint16_t pending_version  = 0;
        uint8_t  pending_flags    = 0;
        uint32_t pending_mg       = 0;

        // Loop minimum: sizeof(kv_commit_rec_t) = 14 bytes ensures we can safely
        // read both rec_len (2 bytes at offset 0) and magic (4 bytes at offset 6)
        // for any record type.  Data records that overlap the sector boundary are
        // caught by the explicit `off + rec_len > KV_SECTOR_SIZE` bounds check
        // inside each branch, so no unsafe memory access can occur.  Using the
        // smaller commit-record minimum (14 < 26) is intentional: commit records
        // near the sector end are valid and must not be missed.
        while ((uint32_t)off + sizeof(kv_commit_rec_t) <= KV_SECTOR_SIZE) {
            const uint8_t *rptr = xip_ptr(page_off + off);
            uint16_t rec_len = *(const uint16_t *)rptr;

            if (rec_len == 0xFFFFu) break;  // end-of-written-data sentinel
            if (rec_len == 0u)      break;  // invalid

            // Both kv_rec_prefix_t and kv_commit_rec_t place their distinguishing
            // magic field at byte offset 6 within the record, immediately after
            // rec_len (2) + mutation_group (4).  A single read dispatches both.
            uint32_t magic = *(const uint32_t *)(rptr + 6u);

            if (magic == KV_COMMIT_MAGIC) {
                // Commit marker: validates that the preceding data record was
                // fully written before power loss.
                if (rec_len < (uint16_t)sizeof(kv_commit_rec_t)) break;
                if ((uint32_t)off + rec_len > KV_SECTOR_SIZE)    break;

                const kv_commit_rec_t *cr = (const kv_commit_rec_t *)rptr;

                // Validate the commit record's own CRC to reject bit-flips.
                uint32_t commit_crc = crc32((const uint8_t *)&cr->mutation_group, 8u);

                if (cr->crc == commit_crc &&
                    has_pending &&
                    cr->mutation_group == pending_mg) {
                    // Mutation group is committed — apply the pending record to
                    // the index, respecting version-based conflict resolution.
                    int f = idx_find_linear(pending_key);
                    uint16_t old_ver = (f >= 0) ? g_versions[(uint32_t)f] : 0;
                    if (f < 0 || pending_version >= old_ver) {
                        if (pending_flags & KV_REC_FLAG_TOMB) {
                            idx_remove(pending_key);
                        } else {
                            idx_set(pending_key, pending_loc, pending_version, pending_flags);
                        }
                    }
                }
                // Clear pending regardless — commit consumed it (or it was invalid).
                has_pending = false;

            } else if (magic == KV_MAGIC) {
                // Data record: stage as pending; a commit marker must follow.
                if (rec_len < (uint16_t)sizeof(kv_rec_prefix_t)) break;
                if ((uint32_t)off + rec_len > KV_SECTOR_SIZE)     break;

                const kv_rec_prefix_t *rp = (const kv_rec_prefix_t *)rptr;
                uint16_t rec_alen = (uint16_t)((rec_len + 3u) & ~3u);

                if (rp->hdr.raw_len   > KV_MAX_VALUE) break;
                if (rp->hdr.store_len > KV_MAX_VALUE) break;

                const uint8_t *stored = xip_ptr(page_off + off + sizeof(kv_rec_prefix_t));
                if (crc32(stored, rp->hdr.store_len) == rp->hdr.checksum) {
                    // Record payload is intact.  Mark pending; will only be
                    // applied to the index when the matching commit arrives.
                    has_pending     = true;
                    pending_key     = rp->hdr.key;
                    pending_loc     = pack_loc(p, off, rec_alen);
                    pending_version = rp->hdr.version;
                    pending_flags   = rp->hdr.flags;
                    pending_mg      = rp->mutation_group;
                }

            } else {
                break;  // unknown magic — assume corrupt tail, stop scanning
            }

            uint16_t alen = (uint16_t)((rec_len + 3u) & ~3u);
            off = (uint16_t)(off + alen);
        }
        // Any remaining has_pending at page-end = uncommitted (power-loss before
        // commit was written).  Silently discard — not applied to index.

        if (off > sizeof(kv_page_hdr_t)) {
            saw_data  = true;
            last_page = p;
            last_off  = off;
        }
    }

    // Seed g_mutation_group above all page sequences seen this boot.
    // Safety invariant: the recovery scanner matches data records with commit
    // markers by mutation_group value ONLY WITHIN a sequential page scan.
    // Because the scan processes records strictly in flash order (data immediately
    // followed by its commit), a new-boot mutation_group value that coincidentally
    // equals an old in-flash value cannot create a false commit match:
    // (a) old committed pairs have already been consumed and cleared from pending,
    // (b) the new write's commit appears only after the new data record, so the
    //     state machine always pairs them correctly regardless of numeric value.
    // Seeding from g_page_sequence ensures new-boot mutation_group IDs start well
    // above any IDs written by the recovered pages: page sequences and mutation_group
    // counters both increment per operation, so g_page_sequence bounds the range of
    // mutation_group IDs that could have been written to the recovered flash pages.
    g_mutation_group = g_page_sequence + 1u;

    if (saw_data) {
        g_write_page = last_page;
        g_write_off  = last_off;
        if (g_write_off >= KV_SECTOR_SIZE) {
            g_write_page = (uint16_t)((g_write_page + 1u) % KV_SECTOR_COUNT);
            g_write_off = sizeof(kv_page_hdr_t);
        }
    }

    g_deadlog_write_page = 0;
    g_deadlog_write_off = sizeof(deadlog_hdr_t);
    g_deadlog_read_page = 0;
    g_deadlog_read_off = sizeof(deadlog_hdr_t);
    for (uint16_t p = 0; p < KV_DEADLOG_SECTORS; p++) {
        uint32_t base = KV_DEADLOG_START + (uint32_t)p * KV_SECTOR_SIZE;
        const deadlog_hdr_t *h = (const deadlog_hdr_t *)xip_ptr(base);
        if (h->magic != KV_DEADLOG_MAGIC) continue;
        uint16_t off = sizeof(deadlog_hdr_t);
        while ((uint32_t)off + sizeof(deadlog_entry_t) <= KV_SECTOR_SIZE) {
            const uint16_t *probe = (const uint16_t *)xip_ptr(base + off);
            if (*probe == 0xFFFFu) break;
            off = (uint16_t)(off + sizeof(deadlog_entry_t));
        }
        g_deadlog_write_page = p;
        g_deadlog_write_off = off;
    }

    printf("[kv] Init: keys=%lu write_page=%u write_off=%u\n",
           (unsigned long)g_count, g_write_page, g_write_off);
}

bool kv_put_if_version(uint32_t key, const uint8_t *value, uint16_t len,
                       uint16_t expected_version, uint16_t *new_version) {
    if (len > KV_MAX_VALUE) return false;

    uint16_t cur_ver = 0;
    uint32_t old_loc = 0;
    bool exists = idx_get(key, &old_loc, &cur_ver, NULL);
    if (expected_version != KV_VERSION_ANY && expected_version != cur_ver) return false;

    // Allocate a fresh mutation group ID for this logical write.
    // The same ID is embedded in the data record and its commit marker so that
    // the recovery scanner can pair them.
    uint32_t mg = ++g_mutation_group;

    uint16_t next_ver = (uint16_t)(cur_ver + 1u);
    uint32_t new_loc = 0;
    if (!append_record(key, value, len, next_ver, 0, mg, &new_loc)) return false;
    if (!idx_set(key, new_loc, next_ver, 0)) return false;

    if (exists) {
        uint16_t old_page = 0;
        unpack_loc(old_loc, &old_page, NULL, NULL);
        deadlog_append(old_page);
    }

    if (new_version) *new_version = next_ver;
    return true;
}

bool kv_put(uint32_t key, const uint8_t *value, uint16_t len) {
    return kv_put_if_version(key, value, len, KV_VERSION_ANY, NULL);
}

const uint8_t *kv_get(uint32_t key, uint16_t *len) {
    static uint8_t scratch[KV_MAX_VALUE];
    uint16_t cap = KV_MAX_VALUE;
    if (!kv_get_copy(key, scratch, &cap, NULL)) return NULL;
    if (len) *len = cap;
    return scratch;
}

bool kv_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    uint32_t loc = 0;
    if (!idx_get(key, &loc, NULL, NULL)) return false;
    return read_record(loc, key, out, len, version);
}

bool kv_delete(uint32_t key) {
    uint16_t cur_ver = 0;
    uint32_t old_loc = 0;
    if (!idx_get(key, &old_loc, &cur_ver, NULL)) return false;

    // Assign a mutation group so recovery can detect a partial delete
    // (tombstone written but commit marker not yet programmed).
    uint32_t mg = ++g_mutation_group;
    uint32_t tomb_loc = 0;
    if (!append_record(key, NULL, 0, (uint16_t)(cur_ver + 1u), KV_REC_FLAG_TOMB, mg, &tomb_loc)) return false;
    idx_remove(key);

    uint16_t old_page = 0;
    unpack_loc(old_loc, &old_page, NULL, NULL);
    deadlog_append(old_page);
    (void)tomb_loc;
    return true;
}

bool kv_exists(uint32_t key) {
    return idx_find_linear(key) >= 0;
}

bool kv_compact_step(void) {
    for (uint32_t spin = 0; spin < KV_DEADLOG_SECTORS; spin++) {
        uint16_t p = (uint16_t)((g_deadlog_read_page + spin) % KV_DEADLOG_SECTORS);
        uint32_t base = KV_DEADLOG_START + (uint32_t)p * KV_SECTOR_SIZE;
        const deadlog_hdr_t *h = (const deadlog_hdr_t *)xip_ptr(base);
        if (h->magic != KV_DEADLOG_MAGIC) continue;

        uint16_t off = (p == g_deadlog_read_page) ? g_deadlog_read_off : sizeof(deadlog_hdr_t);
        if ((uint32_t)off + sizeof(deadlog_entry_t) > KV_SECTOR_SIZE) continue;
        const deadlog_entry_t *e = (const deadlog_entry_t *)xip_ptr(base + off);
        if (e->page_idx == 0xFFFFu) continue;
        if (e->page_idx >= KV_SECTOR_COUNT) {
            g_deadlog_read_page = p;
            g_deadlog_read_off = (uint16_t)(off + sizeof(deadlog_entry_t));
            return false;
        }

        uint16_t dead_page = e->page_idx;
        g_deadlog_read_page = p;
        g_deadlog_read_off = (uint16_t)(off + sizeof(deadlog_entry_t));

        if (dead_page == g_write_page) return false;
        if (page_has_live_refs(dead_page)) return false;

        uint32_t page_off = KV_REGION_START + (uint32_t)dead_page * KV_SECTOR_SIZE;
        const uint32_t *magic = (const uint32_t *)xip_ptr(page_off);
        if (*magic == KV_FREE) return false;

        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(page_off, KV_SECTOR_SIZE);
        restore_interrupts(ints);
        return true;
    }
    return false;
}

uint32_t kv_reclaim(void) {
    uint32_t n = 0;
    while (kv_compact_step()) n++;
    return n;
}

kv_stats_t kv_stats(void) {
    kv_stats_t s = {0, 0, 0, KV_SECTOR_COUNT};
    s.active = g_count;

    bool live_pages[KV_SECTOR_COUNT];
    memset(live_pages, 0, sizeof(live_pages));

    uint32_t indexed_pages = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        uint16_t page = 0;
        unpack_loc(g_locs[i], &page, NULL, NULL);
        if (page < KV_SECTOR_COUNT && !live_pages[page]) {
            live_pages[page] = true;
            indexed_pages++;
        }
    }

    uint32_t header_pages = 0;
    for (uint16_t p = 0; p < KV_SECTOR_COUNT; p++) {
        uint32_t page_off = KV_REGION_START + (uint32_t)p * KV_SECTOR_SIZE;
        const kv_page_hdr_t *ph = (const kv_page_hdr_t *)xip_ptr(page_off);
        if (ph->magic != KV_PAGE_MAGIC_V2) continue;
        header_pages++;
    }

    uint32_t used_pages = (header_pages > indexed_pages) ? header_pages : indexed_pages;
    s.free = (used_pages <= KV_SECTOR_COUNT) ? (KV_SECTOR_COUNT - used_pages) : 0;
    s.dead = (header_pages > indexed_pages) ? (header_pages - indexed_pages) : 0;
    return s;
}

uint32_t kv_range(uint32_t key_prefix, uint32_t prefix_mask,
                  uint32_t *out_keys, uint16_t *out_sectors, uint32_t max_results) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_count && count < max_results; i++) {
        if ((g_keys[i] & prefix_mask) == (key_prefix & prefix_mask)) {
            if (out_keys) out_keys[count] = g_keys[i];
            if (out_sectors) {
                uint16_t page = 0;
                unpack_loc(g_locs[i], &page, NULL, NULL);
                out_sectors[count] = page;
            }
            count++;
        }
    }
    return count;
}

uint32_t kv_record_count(void) {
    return g_count;
}

uint32_t kv_type_counts(uint16_t *out_types, uint32_t *out_counts, uint32_t max_types) {
    uint32_t n = 0;
    uint16_t prev_type = 0xFFFF;

    // g_keys is sorted, so records of the same type are contiguous
    for (uint32_t i = 0; i < g_count; i++) {
        uint16_t t = (uint16_t)(g_keys[i] >> 22);
        if (t == prev_type && n > 0) {
            out_counts[n - 1]++;
        } else {
            if (n >= max_types) break;
            out_types[n] = t;
            out_counts[n] = 1;
            prev_type = t;
            n++;
        }
    }
    return n;
}

// Aliases for kv_store.h — unambiguous names that won't be macro-redirected
bool kvf_put(uint32_t key, const uint8_t *value, uint16_t len) { return kv_put(key, value, len); }
bool kvf_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *ver) { return kv_get_copy(key, out, len, ver); }
bool kvf_delete(uint32_t key) { return kv_delete(key); }
uint32_t kvf_range(uint32_t p, uint32_t m, uint32_t *k, uint16_t *s, uint32_t mx) { return kv_range(p, m, k, s, mx); }
uint32_t kvf_record_count(void) { return kv_record_count(); }