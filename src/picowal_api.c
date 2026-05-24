#include "picowal_api.h"

#include "kv_flash.h"
#define KV_STORE_REDIRECT
#include "kv_store.h"

#include "pico/rand.h"

static bool picowal_api_valid_ref(uint16_t pack, uint32_t card) {
    return pack <= PICOWAL_PACK_MAX && card <= PICOWAL_CARD_MAX;
}

static bool picowal_api_valid_data(const void *data, uint16_t len) {
    return (data || len == 0) && len <= PICOWAL_VALUE_MAX;
}

static bool picowal_api_key_exists(uint32_t key) {
    if (kv_use_sd(key)) return kvsd_exists(key);
    return kv_exists(key);
}

picowal_api_status_t picowal_api_put(uint16_t pack, uint32_t card,
                                     const void *data, uint16_t len) {
    static const uint8_t empty = 0;
    if (!picowal_api_valid_ref(pack, card) || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    return kv_put(picowal_api_key(pack, card), len ? (const uint8_t *)data : &empty, len)
        ? PICOWAL_API_OK
        : PICOWAL_API_IO;
}

picowal_api_status_t picowal_api_put_create(uint16_t pack, uint32_t card,
                                            const void *data, uint16_t len) {
    static const uint8_t empty = 0;
    if (!picowal_api_valid_ref(pack, card) || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    uint32_t key = picowal_api_key(pack, card);
    if (picowal_api_key_exists(key)) {
        return PICOWAL_API_EXISTS;
    }
    return kv_put(key, len ? (const uint8_t *)data : &empty, len)
        ? PICOWAL_API_OK
        : PICOWAL_API_IO;
}

picowal_api_status_t picowal_api_get(uint16_t pack, uint32_t card,
                                     void *out, uint16_t out_cap,
                                     uint16_t *out_len) {
    if (out_len) *out_len = 0;
    if (!picowal_api_valid_ref(pack, card) || !out) {
        return PICOWAL_API_INVALID;
    }
    uint16_t len = out_cap;
    uint32_t key = picowal_api_key(pack, card);
    if (!kv_get_copy(key, (uint8_t *)out, &len, NULL)) {
        return picowal_api_key_exists(key) ? PICOWAL_API_IO : PICOWAL_API_NOT_FOUND;
    }
    if (out_len) *out_len = len;
    return PICOWAL_API_OK;
}

picowal_api_status_t picowal_api_delete(uint16_t pack, uint32_t card) {
    if (!picowal_api_valid_ref(pack, card)) {
        return PICOWAL_API_INVALID;
    }
    return kv_delete(picowal_api_key(pack, card)) ? PICOWAL_API_OK : PICOWAL_API_NOT_FOUND;
}

bool picowal_api_exists(uint16_t pack, uint32_t card) {
    if (!picowal_api_valid_ref(pack, card)) return false;
    return picowal_api_key_exists(picowal_api_key(pack, card));
}

picowal_api_status_t picowal_api_create_random(uint16_t pack,
                                               const void *data, uint16_t len,
                                               uint32_t *out_card) {
    if (out_card) *out_card = 0;
    if (pack > PICOWAL_PACK_MAX || !out_card || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    for (uint8_t attempt = 0; attempt < 16; attempt++) {
        uint32_t card = get_rand_32() & PICOWAL_CARD_MAX;
        picowal_api_status_t st = picowal_api_put_create(pack, card, data, len);
        if (st == PICOWAL_API_OK) {
            *out_card = card;
            return PICOWAL_API_OK;
        }
        if (st != PICOWAL_API_EXISTS) return st;
    }
    return PICOWAL_API_EXISTS;
}

uint32_t picowal_api_list(uint16_t pack, uint32_t *out_cards, uint32_t max_cards) {
    if (pack > PICOWAL_PACK_MAX || !out_cards || max_cards == 0) return 0;

    uint32_t count = kv_range(picowal_api_key(pack, 0), 0xFFC00000u,
                              out_cards, NULL, max_cards);
    for (uint32_t i = 0; i < count; i++) {
        out_cards[i] = picowal_api_key_card(out_cards[i]);
    }
    return count;
}
