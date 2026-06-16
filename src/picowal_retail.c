#include "picowal_retail.h"

#if !defined(PICOWAL_HOST)
#error "picowal_retail is a host/server-side primitive; do not compile it into Pico firmware."
#endif

#include "picowal_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    picowal_retail_product_t product;
} retail_card_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    char visitor_id[32];
    char event_type[32];
    char product_id[PICOWAL_RETAIL_ID_MAX + 1u];
} retail_event_card_t;

#define RETAIL_PRODUCT_MAGIC 0x5052574Cu /* "PRWL" */
#define RETAIL_EVENT_MAGIC   0x4552574Cu /* "ERWL" */

static uint16_t copy_text(char *dst, uint16_t cap, const char *src) {
    uint16_t n = 0;
    if (!dst || cap == 0 || !src) return 0;
    while (src[n] && n + 1u < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

static uint32_t fnv1a(const char *text) {
    uint32_t hash = 2166136261u;
    if (!text) return 1u;
    for (uint32_t i = 0; text[i]; i++) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    hash &= PICOWAL_CARD_MAX;
    return hash ? hash : 1u;
}

uint32_t picowal_retail_card_for_id(const char *id) {
    return fnv1a(id);
}

static void retail_vector(const char *text, float *vector, uint16_t dims) {
    if (!vector || dims == 0) return;
    for (uint16_t i = 0; i < dims; i++) vector[i] = 0.0f;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; text && text[i]; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            hash = 2166136261u;
            continue;
        }
        hash ^= (uint8_t)c;
        hash *= 16777619u;
        vector[hash % dims] += (hash & 0x80000000u) ? 1.0f : -1.0f;
    }
}

static void product_text(const picowal_retail_product_t *product, char *out, size_t cap) {
    snprintf(out, cap, "%s %s %s %s %s",
             product->title, product->description, product->category,
             product->brand, product->tags);
}

void picowal_retail_init(picowal_retail_t *retail,
                         uint16_t product_pack,
                         uint16_t index_pack,
                         uint32_t index_base_card,
                         uint16_t journal_pack,
                         uint32_t journal_base_card) {
    if (!retail) return;
    memset(retail, 0, sizeof(*retail));
    retail->product_pack = product_pack;
    retail->index_pack = index_pack;
    retail->index_base_card = index_base_card;
    retail->journal_pack = journal_pack;
    retail->journal_base_card = journal_base_card;
    picowal_search_init(&retail->search);
    picowal_search_configure(&retail->search, "retail-products", 1, 1, 0);
    if (picowal_search_load_from_pack(&retail->search, index_pack, index_base_card) != PICOWAL_SEARCH_OK) {
        picowal_search_init(&retail->search);
        picowal_search_configure(&retail->search, "retail-products", 1, 1, 0);
    }
    picowal_search_journal_replay_from_pack(&retail->search, journal_pack, journal_base_card);
}

picowal_search_status_t picowal_retail_upsert(picowal_retail_t *retail,
                                              const picowal_retail_product_t *product) {
    if (!retail || !product || !product->id[0] || !product->title[0]) return PICOWAL_SEARCH_INVALID;
    uint32_t card = picowal_retail_card_for_id(product->id);
    retail_card_t row = {
        .magic = RETAIL_PRODUCT_MAGIC,
        .version = 1,
        .reserved = 0,
        .product = *product,
    };
    picowal_api_status_t api = picowal_api_put(retail->product_pack, card, &row, sizeof(row));
    if (api != PICOWAL_API_OK) return PICOWAL_SEARCH_IO;

    char text[512];
    float vector[PICOWAL_RETAIL_VECTOR_DIMS];
    product_text(product, text, sizeof(text));
    retail_vector(text, vector, PICOWAL_RETAIL_VECTOR_DIMS);
    picowal_search_status_t st = picowal_search_journal_upsert_to_pack(
        &retail->search, retail->journal_pack, retail->journal_base_card,
        retail->product_pack, card, text, vector, PICOWAL_RETAIL_VECTOR_DIMS);
    if (st != PICOWAL_SEARCH_OK) return st;
    if ((st = picowal_search_journal_facet_to_pack(&retail->search, retail->journal_pack,
                                                   retail->journal_base_card, retail->product_pack,
                                                   card, "category", product->category)) != PICOWAL_SEARCH_OK) return st;
    if ((st = picowal_search_journal_facet_to_pack(&retail->search, retail->journal_pack,
                                                   retail->journal_base_card, retail->product_pack,
                                                   card, "brand", product->brand)) != PICOWAL_SEARCH_OK) return st;
    if ((st = picowal_search_journal_number_to_pack(&retail->search, retail->journal_pack,
                                                    retail->journal_base_card, retail->product_pack,
                                                    card, "price", product->price)) != PICOWAL_SEARCH_OK) return st;
    return picowal_search_save_to_pack(&retail->search, retail->index_pack, retail->index_base_card);
}

picowal_search_status_t picowal_retail_get(picowal_retail_t *retail,
                                           const char *id,
                                           picowal_retail_product_t *out_product) {
    if (!retail || !id || !out_product) return PICOWAL_SEARCH_INVALID;
    retail_card_t row;
    uint16_t len = sizeof(row);
    picowal_api_status_t st = picowal_api_get(retail->product_pack, picowal_retail_card_for_id(id),
                                              &row, sizeof(row), &len);
    if (st == PICOWAL_API_NOT_FOUND) return PICOWAL_SEARCH_NOT_FOUND;
    if (st != PICOWAL_API_OK || len != sizeof(row) || row.magic != RETAIL_PRODUCT_MAGIC) {
        return PICOWAL_SEARCH_CORRUPT;
    }
    *out_product = row.product;
    return PICOWAL_SEARCH_OK;
}

uint32_t picowal_retail_list(picowal_retail_t *retail,
                             picowal_retail_product_t *out_products,
                             uint32_t max_products) {
    if (!retail || !out_products || max_products == 0) return 0;
    uint32_t cards[128];
    uint32_t count = picowal_api_list(retail->product_pack, cards, 128);
    uint32_t out = 0;
    for (uint32_t i = 0; i < count && out < max_products; i++) {
        retail_card_t row;
        uint16_t len = sizeof(row);
        if (picowal_api_get(retail->product_pack, cards[i], &row, sizeof(row), &len) == PICOWAL_API_OK &&
            len == sizeof(row) && row.magic == RETAIL_PRODUCT_MAGIC) {
            out_products[out++] = row.product;
        }
    }
    return out;
}

static const picowal_retail_product_t demo_products[] = {
    { "aurora-shell", "Aurora Storm Shell Jacket", "Breathable waterproof shell with taped seams", "outerwear", "Contoso Trail", "waterproof hiking rain", 129.99f, 142 },
    { "ridge-boot", "RidgeWalker Leather Hiking Boot", "Leather waterproof boot with aggressive grip", "footwear", "Northwind", "boots waterproof trail", 154.0f, 88 },
    { "merino-base", "Merino Thermal Base Layer", "Soft merino blend for winter starts", "base-layers", "Fabrikam Alpine", "merino thermal winter", 58.5f, 210 },
    { "summit-pack", "Summit 32L Day Pack", "Ventilated pack with hydration sleeve", "packs", "Contoso Trail", "backpack hydration hiking", 94.99f, 64 },
    { "camp-lantern", "LumaCamp Rechargeable Lantern", "USB-C lantern with warm dimming modes", "camping", "Litware Camp", "lantern camping rechargeable", 39.99f, 300 },
    { "down-jacket", "Nimbus Packable Down Jacket", "Warm packable down jacket for alpine starts", "outerwear", "Fabrikam Alpine", "down winter jacket", 199.99f, 57 },
};

picowal_search_status_t picowal_retail_ingest_demo(picowal_retail_t *retail) {
    if (!retail) return PICOWAL_SEARCH_INVALID;
    for (uint32_t i = 0; i < sizeof(demo_products) / sizeof(demo_products[0]); i++) {
        picowal_search_status_t st = picowal_retail_upsert(retail, &demo_products[i]);
        if (st != PICOWAL_SEARCH_OK) return st;
    }
    return PICOWAL_SEARCH_OK;
}

static size_t append_json(char *out, size_t cap, size_t pos, const char *fmt, ...) {
    if (!out || pos >= cap) return pos;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out + pos, cap - pos, fmt, args);
    va_end(args);
    if (n < 0) return pos;
    size_t next = pos + (size_t)n;
    return next >= cap ? cap - 1u : next;
}

static size_t product_json(char *out, size_t cap, size_t pos, const picowal_retail_product_t *p) {
    return append_json(out, cap, pos,
        "{\"id\":\"%s\",\"title\":\"%s\",\"description\":\"%s\",\"category\":\"%s\",\"brand\":\"%s\",\"tags\":\"%s\",\"price\":%.2f,\"inventory\":%lu}",
        p->id, p->title, p->description, p->category, p->brand, p->tags,
        (double)p->price, (unsigned long)p->inventory);
}

picowal_search_status_t picowal_retail_products_json(picowal_retail_t *retail,
                                                     char *out_json,
                                                     size_t out_cap) {
    if (!retail || !out_json || out_cap == 0) return PICOWAL_SEARCH_INVALID;
    picowal_retail_product_t products[64];
    uint32_t count = picowal_retail_list(retail, products, 64);
    size_t pos = append_json(out_json, out_cap, 0, "{\"products\":[");
    for (uint32_t i = 0; i < count; i++) {
        if (i) pos = append_json(out_json, out_cap, pos, ",");
        pos = product_json(out_json, out_cap, pos, &products[i]);
    }
    append_json(out_json, out_cap, pos, "],\"totalSize\":%lu}", (unsigned long)count);
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_retail_product_json(picowal_retail_t *retail,
                                                    const char *id,
                                                    char *out_json,
                                                    size_t out_cap) {
    picowal_retail_product_t product;
    picowal_search_status_t st = picowal_retail_get(retail, id, &product);
    if (st != PICOWAL_SEARCH_OK) return st;
    product_json(out_json, out_cap, 0, &product);
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_retail_search_json(picowal_retail_t *retail,
                                                   const char *query,
                                                   char *out_json,
                                                   size_t out_cap) {
    if (!retail || !out_json || out_cap == 0) return PICOWAL_SEARCH_INVALID;
    float vector[PICOWAL_RETAIL_VECTOR_DIMS];
    retail_vector(query, vector, PICOWAL_RETAIL_VECTOR_DIMS);
    picowal_search_response_t res;
    picowal_search_request_t req = {
        .query_text = query ? query : "",
        .query_vector = vector,
        .query_vector_dims = PICOWAL_RETAIL_VECTOR_DIMS,
        .limit = 16,
        .candidate_limit = 32,
    };
    picowal_search_status_t st = picowal_search_query(&retail->search, &req, &res);
    if (st != PICOWAL_SEARCH_OK) return st;
    picowal_search_facet_count_t facets[16];
    uint16_t facet_count = 0;
    picowal_search_facets(&retail->search, "category", facets, 16, &facet_count);

    size_t pos = append_json(out_json, out_cap, 0, "{\"results\":[");
    for (uint16_t i = 0; i < res.hit_count; i++) {
        picowal_retail_product_t p;
        uint32_t card = picowal_api_key_card(res.hits[i].key);
        retail_card_t row;
        uint16_t len = sizeof(row);
        if (picowal_api_get(retail->product_pack, card, &row, sizeof(row), &len) != PICOWAL_API_OK ||
            len != sizeof(row) || row.magic != RETAIL_PRODUCT_MAGIC) continue;
        p = row.product;
        if (i) pos = append_json(out_json, out_cap, pos, ",");
        pos = append_json(out_json, out_cap, pos, "{\"id\":\"%s\",\"score\":%.4f,\"product\":",
                          p.id, (double)res.hits[i].score);
        pos = product_json(out_json, out_cap, pos, &p);
        pos = append_json(out_json, out_cap, pos, "}");
    }
    pos = append_json(out_json, out_cap, pos, "],\"totalSize\":%u,\"facets\":{\"category\":[", res.hit_count);
    for (uint16_t i = 0; i < facet_count; i++) {
        if (i) pos = append_json(out_json, out_cap, pos, ",");
        pos = append_json(out_json, out_cap, pos, "{\"value\":\"%s\",\"count\":%lu}",
                          facets[i].value, (unsigned long)facets[i].count);
    }
    append_json(out_json, out_cap, pos, "]}}");
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_retail_recommend_json(picowal_retail_t *retail,
                                                      const char *id,
                                                      char *out_json,
                                                      size_t out_cap) {
    picowal_retail_product_t product;
    picowal_search_status_t st = picowal_retail_get(retail, id, &product);
    if (st != PICOWAL_SEARCH_OK) return st;
    char text[512];
    product_text(&product, text, sizeof(text));
    return picowal_retail_search_json(retail, text, out_json, out_cap);
}

picowal_search_status_t picowal_retail_record_event(picowal_retail_t *retail,
                                                    uint16_t event_pack,
                                                    const char *visitor_id,
                                                    const char *event_type,
                                                    const char *product_id) {
    (void)retail;
    retail_event_card_t event;
    memset(&event, 0, sizeof(event));
    event.magic = RETAIL_EVENT_MAGIC;
    event.version = 1;
    copy_text(event.visitor_id, sizeof(event.visitor_id), visitor_id);
    copy_text(event.event_type, sizeof(event.event_type), event_type);
    copy_text(event.product_id, sizeof(event.product_id), product_id);
    uint32_t card = fnv1a(visitor_id) ^ fnv1a(product_id) ^ fnv1a(event_type);
    card &= PICOWAL_CARD_MAX;
    if (!card) card = 1;
    return picowal_api_put(event_pack, card, &event, sizeof(event)) == PICOWAL_API_OK
        ? PICOWAL_SEARCH_OK
        : PICOWAL_SEARCH_IO;
}
