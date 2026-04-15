#ifndef KV_STORE_H
#define KV_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "kv_flash.h"
#include "kv_sd.h"

// ============================================================
// Unified KV Store — routes to flash or SD by pack ordinal
//
// System packs (0=metadata, 1=users): always flash
// User packs (2+): SD when ready, fallback to flash
//
// Key layout: [pack:10 bits][card:22 bits]
// Pack extracted as: (key >> 22) & 0x3FF
// ============================================================

#define KV_PACK_FROM_KEY(key) (((key) >> 22) & 0x3FF)
#define KV_SYSTEM_PACK_MAX  1   // packs 0 and 1 stay on flash

static inline bool kv_use_sd(uint32_t key) {
    return kvsd_ready() && KV_PACK_FROM_KEY(key) > KV_SYSTEM_PACK_MAX;
}

static inline bool kv_use_sd_pack(uint16_t pack) {
    return kvsd_ready() && pack > KV_SYSTEM_PACK_MAX;
}

// ---- Unified API (use explicit kvf_ prefix for flash calls) ----

// Forward declarations with unambiguous names for flash functions
// (these are the real kv_flash.h functions before any macro redirect)
extern bool kvf_put(uint32_t key, const uint8_t *value, uint16_t len);
extern bool kvf_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *ver);
extern bool kvf_delete(uint32_t key);
extern uint32_t kvf_range(uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint16_t *out_sectors, uint32_t max);
extern uint32_t kvf_record_count(void);

static inline bool kv_store_put(uint32_t key, const uint8_t *value, uint16_t len) {
    if (kv_use_sd(key)) {
        if (len > KVSD_MAX_PAYLOAD) return false;
        return kvsd_put(key, value, len);
    }
    return kvf_put(key, value, len);
}

static inline bool kv_store_get_copy(uint32_t key, uint8_t *out, uint16_t *len, uint16_t *ver) {
    if (kv_use_sd(key))
        return kvsd_get_copy(key, out, len, ver);
    return kvf_get_copy(key, out, len, ver);
}

static inline bool kv_store_delete(uint32_t key) {
    if (kv_use_sd(key))
        return kvsd_delete(key);
    return kvf_delete(key);
}

static inline uint32_t kv_store_range(uint32_t prefix, uint32_t mask,
                                       uint32_t *out_keys, uint16_t *out_sectors,
                                       uint32_t max) {
    uint16_t pack = KV_PACK_FROM_KEY(prefix);
    if (kv_use_sd_pack(pack))
        return kvsd_range(prefix, mask, out_keys, out_sectors, max);
    return kvf_range(prefix, mask, out_keys, out_sectors, max);
}

static inline uint32_t kv_store_record_count(void) {
    uint32_t n = kvf_record_count();
    if (kvsd_ready()) n += kvsd_record_count();
    return n;
}

// ---- Redirect macros ----
// Include kv_store.h AFTER kv_flash.h in consumer files.
// Define KV_STORE_REDIRECT before including to enable the macros.
// Do NOT include in kv_flash.c or kv_sd.c.
#ifdef KV_STORE_REDIRECT
#undef kv_get_copy
#undef kv_put
#undef kv_delete
#undef kv_range
#define kv_get_copy  kv_store_get_copy
#define kv_put       kv_store_put
#define kv_delete    kv_store_delete
#define kv_range     kv_store_range
#endif

#endif
