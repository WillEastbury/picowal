#include "picowal_store_sd.h"

#include "kv_sd.h"

#include <stddef.h>

static bool sd_ready(void *ctx) {
    (void)ctx;
    return kvsd_ready();
}

static bool sd_put(void *ctx, uint32_t key, const uint8_t *value, uint16_t len) {
    (void)ctx;
    return kvsd_put(key, value, len);
}

static bool sd_get_copy(void *ctx, uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    (void)ctx;
    return kvsd_get_copy(key, out, len, version);
}

static bool sd_delete(void *ctx, uint32_t key) {
    (void)ctx;
    return kvsd_delete(key);
}

static bool sd_exists(void *ctx, uint32_t key) {
    (void)ctx;
    return kvsd_exists(key);
}

static uint32_t sd_range(void *ctx, uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint32_t max) {
    (void)ctx;
    return kvsd_range(prefix, mask, out_keys, NULL, max);
}

static const picowal_store_ops_t sd_ops = {
    .name = "sd",
    .ready = sd_ready,
    .put = sd_put,
    .get_copy = sd_get_copy,
    .del = sd_delete,
    .exists = sd_exists,
    .range = sd_range,
};

const picowal_store_t picowal_sd_store = {
    .ops = &sd_ops,
    .ctx = NULL,
};
