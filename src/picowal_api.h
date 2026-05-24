#ifndef PICOWAL_API_H
#define PICOWAL_API_H

#include <stdbool.h>
#include <stdint.h>

#define PICOWAL_PACK_MAX   0x03FFu
#define PICOWAL_CARD_MAX   0x3FFFFFu
#define PICOWAL_VALUE_MAX  508u

typedef enum {
    PICOWAL_API_OK = 0,
    PICOWAL_API_NOT_FOUND,
    PICOWAL_API_EXISTS,
    PICOWAL_API_INVALID,
    PICOWAL_API_IO,
} picowal_api_status_t;

static inline uint32_t picowal_api_key(uint16_t pack, uint32_t card) {
    return ((uint32_t)(pack & PICOWAL_PACK_MAX) << 22) | (card & PICOWAL_CARD_MAX);
}

static inline uint16_t picowal_api_key_pack(uint32_t key) {
    return (uint16_t)((key >> 22) & PICOWAL_PACK_MAX);
}

static inline uint32_t picowal_api_key_card(uint32_t key) {
    return key & PICOWAL_CARD_MAX;
}

picowal_api_status_t picowal_api_put(uint16_t pack, uint32_t card,
                                     const void *data, uint16_t len);
picowal_api_status_t picowal_api_put_create(uint16_t pack, uint32_t card,
                                            const void *data, uint16_t len);
picowal_api_status_t picowal_api_get(uint16_t pack, uint32_t card,
                                     void *out, uint16_t out_cap,
                                     uint16_t *out_len);
picowal_api_status_t picowal_api_delete(uint16_t pack, uint32_t card);
bool picowal_api_exists(uint16_t pack, uint32_t card);
picowal_api_status_t picowal_api_create_random(uint16_t pack,
                                               const void *data, uint16_t len,
                                               uint32_t *out_card);
uint32_t picowal_api_list(uint16_t pack, uint32_t *out_cards, uint32_t max_cards);

#endif
