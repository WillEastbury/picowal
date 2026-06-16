#ifndef PICOWAL_RETAIL_H
#define PICOWAL_RETAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "picowal_search.h"

#define PICOWAL_RETAIL_ID_MAX          31u
#define PICOWAL_RETAIL_TITLE_MAX       95u
#define PICOWAL_RETAIL_DESCRIPTION_MAX 159u
#define PICOWAL_RETAIL_FIELD_MAX       31u
#define PICOWAL_RETAIL_TAGS_MAX        95u
#define PICOWAL_RETAIL_JSON_MAX        8192u
#define PICOWAL_RETAIL_VECTOR_DIMS     16u

typedef struct {
    char id[PICOWAL_RETAIL_ID_MAX + 1u];
    char title[PICOWAL_RETAIL_TITLE_MAX + 1u];
    char description[PICOWAL_RETAIL_DESCRIPTION_MAX + 1u];
    char category[PICOWAL_RETAIL_FIELD_MAX + 1u];
    char brand[PICOWAL_RETAIL_FIELD_MAX + 1u];
    char tags[PICOWAL_RETAIL_TAGS_MAX + 1u];
    float price;
    uint32_t inventory;
} picowal_retail_product_t;

typedef struct {
    uint16_t product_pack;
    uint16_t index_pack;
    uint32_t index_base_card;
    uint16_t journal_pack;
    uint32_t journal_base_card;
    picowal_search_index_t search;
} picowal_retail_t;

void picowal_retail_init(picowal_retail_t *retail,
                         uint16_t product_pack,
                         uint16_t index_pack,
                         uint32_t index_base_card,
                         uint16_t journal_pack,
                         uint32_t journal_base_card);
uint32_t picowal_retail_card_for_id(const char *id);
picowal_search_status_t picowal_retail_upsert(picowal_retail_t *retail,
                                              const picowal_retail_product_t *product);
picowal_search_status_t picowal_retail_get(picowal_retail_t *retail,
                                           const char *id,
                                           picowal_retail_product_t *out_product);
uint32_t picowal_retail_list(picowal_retail_t *retail,
                             picowal_retail_product_t *out_products,
                             uint32_t max_products);
picowal_search_status_t picowal_retail_ingest_demo(picowal_retail_t *retail);
picowal_search_status_t picowal_retail_search_json(picowal_retail_t *retail,
                                                   const char *query,
                                                   char *out_json,
                                                   size_t out_cap);
picowal_search_status_t picowal_retail_products_json(picowal_retail_t *retail,
                                                     char *out_json,
                                                     size_t out_cap);
picowal_search_status_t picowal_retail_product_json(picowal_retail_t *retail,
                                                    const char *id,
                                                    char *out_json,
                                                    size_t out_cap);
picowal_search_status_t picowal_retail_recommend_json(picowal_retail_t *retail,
                                                      const char *id,
                                                      char *out_json,
                                                      size_t out_cap);
picowal_search_status_t picowal_retail_record_event(picowal_retail_t *retail,
                                                    uint16_t event_pack,
                                                    const char *visitor_id,
                                                    const char *event_type,
                                                    const char *product_id);

#endif
