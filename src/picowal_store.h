#ifndef PICOWAL_STORE_H
#define PICOWAL_STORE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct picowal_store_ops {
    const char *name;
    bool (*ready)(void *ctx);
    bool (*put)(void *ctx, uint32_t key, const uint8_t *value, uint16_t len);
    bool (*get_copy)(void *ctx, uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version);
    bool (*del)(void *ctx, uint32_t key);
    bool (*exists)(void *ctx, uint32_t key);
    uint32_t (*range)(void *ctx, uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint32_t max);
} picowal_store_ops_t;

typedef struct picowal_store {
    const picowal_store_ops_t *ops;
    void *ctx;
} picowal_store_t;

static inline bool picowal_store_ready(const picowal_store_t *store) {
    return store && store->ops && store->ops->ready && store->ops->ready(store->ctx);
}

static inline bool picowal_store_put(const picowal_store_t *store, uint32_t key,
                                     const uint8_t *value, uint16_t len) {
    return store && store->ops && store->ops->put && store->ops->put(store->ctx, key, value, len);
}

static inline bool picowal_store_get_copy(const picowal_store_t *store, uint32_t key,
                                          uint8_t *out, uint16_t *len, uint16_t *version) {
    return store && store->ops && store->ops->get_copy &&
           store->ops->get_copy(store->ctx, key, out, len, version);
}

static inline bool picowal_store_delete(const picowal_store_t *store, uint32_t key) {
    return store && store->ops && store->ops->del && store->ops->del(store->ctx, key);
}

static inline bool picowal_store_exists(const picowal_store_t *store, uint32_t key) {
    return store && store->ops && store->ops->exists && store->ops->exists(store->ctx, key);
}

static inline uint32_t picowal_store_range(const picowal_store_t *store, uint32_t prefix,
                                           uint32_t mask, uint32_t *out_keys, uint32_t max) {
    if (!store || !store->ops || !store->ops->range || !out_keys || max == 0) return 0;
    return store->ops->range(store->ctx, prefix, mask, out_keys, max);
}

#endif
