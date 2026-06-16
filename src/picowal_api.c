#include "picowal_api.h"

#if !defined(PICOWAL_NO_DEFAULT_STORE)
#include "picowal_store_sd.h"
#endif

#if defined(PICOWAL_HOST)
#include <time.h>
#else
#include "pico/rand.h"
#endif

static const picowal_store_t *g_store =
#if defined(PICOWAL_NO_DEFAULT_STORE)
    NULL;
#else
    &picowal_sd_store;
#endif

void picowal_api_set_store(const picowal_store_t *store) {
    g_store = store;
}

const picowal_store_t *picowal_api_get_store(void) {
    return g_store;
}

static bool picowal_api_valid_ref(uint16_t pack, uint32_t card) {
    return pack >= 2 && pack <= PICOWAL_PACK_MAX && card <= PICOWAL_CARD_MAX;
}

static bool picowal_api_valid_data(const void *data, uint16_t len) {
    return (data || len == 0) && len <= PICOWAL_VALUE_MAX;
}

static bool picowal_api_key_exists(uint32_t key) {
    const picowal_store_t *store = picowal_api_get_store();
    return picowal_store_ready(store) && picowal_store_exists(store, key);
}

static uint32_t picowal_api_rand32(void) {
#if defined(PICOWAL_HOST)
    static uint32_t state = 0;
    if (state == 0) state = (uint32_t)time(NULL) ^ 0xA5A55A5Au;
    state = state * 1664525u + 1013904223u;
    return state;
#else
    return get_rand_32();
#endif
}

picowal_api_status_t picowal_api_put(uint16_t pack, uint32_t card,
                                     const void *data, uint16_t len) {
    static const uint8_t empty = 0;
    const picowal_store_t *store = picowal_api_get_store();
    if (!picowal_api_valid_ref(pack, card) || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    if (!picowal_store_ready(store)) {
        return PICOWAL_API_NOT_READY;
    }
    return picowal_store_put(store, picowal_api_key(pack, card), len ? (const uint8_t *)data : &empty, len)
        ? PICOWAL_API_OK
        : PICOWAL_API_IO;
}

picowal_api_status_t picowal_api_put_create(uint16_t pack, uint32_t card,
                                            const void *data, uint16_t len) {
    static const uint8_t empty = 0;
    const picowal_store_t *store = picowal_api_get_store();
    if (!picowal_api_valid_ref(pack, card) || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    if (!picowal_store_ready(store)) {
        return PICOWAL_API_NOT_READY;
    }
    uint32_t key = picowal_api_key(pack, card);
    if (picowal_api_key_exists(key)) {
        return PICOWAL_API_EXISTS;
    }
    return picowal_store_put(store, key, len ? (const uint8_t *)data : &empty, len)
        ? PICOWAL_API_OK
        : PICOWAL_API_IO;
}

picowal_api_status_t picowal_api_get(uint16_t pack, uint32_t card,
                                     void *out, uint16_t out_cap,
                                     uint16_t *out_len) {
    const picowal_store_t *store = picowal_api_get_store();
    if (out_len) *out_len = 0;
    if (!picowal_api_valid_ref(pack, card) || !out) {
        return PICOWAL_API_INVALID;
    }
    if (!picowal_store_ready(store)) {
        return PICOWAL_API_NOT_READY;
    }
    uint16_t len = out_cap;
    uint32_t key = picowal_api_key(pack, card);
    if (!picowal_store_get_copy(store, key, (uint8_t *)out, &len, NULL)) {
        return picowal_api_key_exists(key) ? PICOWAL_API_IO : PICOWAL_API_NOT_FOUND;
    }
    if (out_len) *out_len = len;
    return PICOWAL_API_OK;
}

picowal_api_status_t picowal_api_delete(uint16_t pack, uint32_t card) {
    const picowal_store_t *store = picowal_api_get_store();
    if (!picowal_api_valid_ref(pack, card)) {
        return PICOWAL_API_INVALID;
    }
    if (!picowal_store_ready(store)) {
        return PICOWAL_API_NOT_READY;
    }
    return picowal_store_delete(store, picowal_api_key(pack, card)) ? PICOWAL_API_OK : PICOWAL_API_NOT_FOUND;
}

bool picowal_api_exists(uint16_t pack, uint32_t card) {
    const picowal_store_t *store = picowal_api_get_store();
    if (!picowal_api_valid_ref(pack, card) || !picowal_store_ready(store)) return false;
    return picowal_api_key_exists(picowal_api_key(pack, card));
}

picowal_api_status_t picowal_api_create_random(uint16_t pack,
                                               const void *data, uint16_t len,
                                               uint32_t *out_card) {
    const picowal_store_t *store = picowal_api_get_store();
    if (out_card) *out_card = 0;
    if (pack < 2 || pack > PICOWAL_PACK_MAX || !out_card || !picowal_api_valid_data(data, len)) {
        return PICOWAL_API_INVALID;
    }
    if (!picowal_store_ready(store)) {
        return PICOWAL_API_NOT_READY;
    }
    for (uint8_t attempt = 0; attempt < 16; attempt++) {
        uint32_t card = picowal_api_rand32() & PICOWAL_CARD_MAX;
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
    const picowal_store_t *store = picowal_api_get_store();
    if (pack < 2 || pack > PICOWAL_PACK_MAX || !out_cards || max_cards == 0 ||
        !picowal_store_ready(store)) {
        return 0;
    }

    uint32_t count = picowal_store_range(store, picowal_api_key(pack, 0), 0xFFC00000u,
                                         out_cards, max_cards);
    for (uint32_t i = 0; i < count; i++) {
        out_cards[i] = picowal_api_key_card(out_cards[i]);
    }
    return count;
}
