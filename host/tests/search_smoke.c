#define _GNU_SOURCE

#include "picowal_search.h"
#include "picowal_store_fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static float semantic_score(const char *query, const char *document, void *ctx) {
    (void)ctx;
    if (strstr(query, "waterproof") && strstr(document, "waterproof hiking shell")) return 2.0f;
    return 0.0f;
}

static int expect_key(const picowal_search_response_t *response, uint16_t idx, uint16_t pack, uint32_t card) {
    if (idx >= response->hit_count) return 0;
    return response->hits[idx].key == picowal_api_key(pack, card);
}

static bool extract_raw_text(uint32_t key, const uint8_t *value, uint16_t value_len,
                             char *out_text, uint16_t out_text_cap,
                             float *out_vector, uint16_t *inout_vector_dims,
                             void *ctx) {
    (void)ctx;
    uint32_t card = picowal_api_key_card(key);
    uint16_t copy = value_len < (uint16_t)(out_text_cap - 1u) ? value_len : (uint16_t)(out_text_cap - 1u);
    memcpy(out_text, value, copy);
    out_text[copy] = 0;
    if (*inout_vector_dims < 2u) return false;
    out_vector[0] = card == 11u ? 1.0f : 0.0f;
    out_vector[1] = card == 12u ? 1.0f : 0.0f;
    *inout_vector_dims = 2;
    return true;
}

int main(void) {
    static picowal_search_index_t index;
    picowal_search_init(&index);
    if (picowal_search_configure(&index, "retail-products", 7, 42, 0) != PICOWAL_SEARCH_OK) return 1;

    const float jacket_vec[] = {1.0f, 0.0f, 0.1f, 0.0f};
    const float boots_vec[] = {0.0f, 1.0f, 0.0f, 0.1f};
    const float shell_vec[] = {0.95f, 0.0f, 0.2f, 0.0f};
    const float query_vec[] = {1.0f, 0.0f, 0.0f, 0.0f};

    if (picowal_search_upsert(&index, 2, 1, "blue waterproof jacket rain hiking", jacket_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_upsert(&index, 2, 2, "leather city boots smart casual", boots_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_upsert(&index, 2, 3, "waterproof hiking shell premium trail", shell_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_facet(&index, 2, 1, "category", "outerwear") != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_facet(&index, 2, 2, "category", "footwear") != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_facet(&index, 2, 3, "category", "outerwear") != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_number(&index, 2, 1, "price", 89.0f) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_number(&index, 2, 2, "price", 129.0f) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_set_number(&index, 2, 3, "price", 159.0f) != PICOWAL_SEARCH_OK) return 1;

    picowal_search_facet_count_t facet_counts[4];
    uint16_t facet_count = 0;
    if (picowal_search_facets(&index, "category", facet_counts, 4, &facet_count) != PICOWAL_SEARCH_OK) return 1;
    if (facet_count != 2) return 1;
    uint32_t outerwear = 0;
    uint32_t footwear = 0;
    for (uint16_t i = 0; i < facet_count; i++) {
        if (strcmp(facet_counts[i].value, "outerwear") == 0) outerwear = facet_counts[i].count;
        if (strcmp(facet_counts[i].value, "footwear") == 0) footwear = facet_counts[i].count;
    }
    if (outerwear != 2 || footwear != 1) return 1;

    uint32_t range_keys[4];
    uint16_t range_count = 0;
    if (picowal_search_range(&index, "price", 80.0f, 130.0f, range_keys, 4, &range_count) != PICOWAL_SEARCH_OK) return 1;
    if (range_count != 2) return 1;

    picowal_search_response_t response;
    picowal_search_request_t request = {
        .query_text = "waterproof jacket",
        .query_vector = query_vec,
        .query_vector_dims = 4,
        .limit = 3,
        .candidate_limit = 3,
        .semantic_weight = 1.0f,
        .semantic_score = semantic_score,
    };
    if (picowal_search_query(&index, &request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 2) return 1;
    if (!expect_key(&response, 0, 2, 3)) return 1;
    if (!expect_key(&response, 1, 2, 1)) return 1;
    if (response.plan.lexical_candidates != 2) return 1;
    if (response.plan.vector_candidates != 2) return 1;
    if (response.plan.semantic_reranked != 2) return 1;
    if (!response.plan.used_vector_ann) return 1;
    if (response.hits[0].semantic_score < 1.9f) return 1;
    if (response.hits[1].bm25_score <= 0.0f) return 1;

    picowal_search_request_t vector_only = {
        .query_text = "",
        .query_vector = boots_vec,
        .query_vector_dims = 4,
        .limit = 1,
        .candidate_limit = 2,
    };
    if (picowal_search_query(&index, &vector_only, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 1 || !expect_key(&response, 0, 2, 2)) return 1;
    if (fabsf(response.hits[0].vector_score - 1.0f) > 0.001f) return 1;

    if (picowal_search_upsert(&index, 2, 1, "orange insulated parka", jacket_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    picowal_search_request_t blue_request = {
        .query_text = "blue",
        .limit = 3,
    };
    if (picowal_search_query(&index, &blue_request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 0) return 1;
    if (picowal_search_upsert(&index, 2, 1, "blue waterproof jacket rain hiking", jacket_vec, 4) != PICOWAL_SEARCH_OK) return 1;

    if (picowal_search_delete(&index, 2, 3) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_facets(&index, "category", facet_counts, 4, &facet_count) != PICOWAL_SEARCH_OK) return 1;
    outerwear = 0;
    for (uint16_t i = 0; i < facet_count; i++) {
        if (strcmp(facet_counts[i].value, "outerwear") == 0) outerwear = facet_counts[i].count;
    }
    if (outerwear != 1) return 1;
    if (picowal_search_query(&index, &request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 1 || !expect_key(&response, 0, 2, 1)) return 1;

    char root[] = "/tmp/picowal-search-smoke-XXXXXX";
    if (!mkdtemp(root)) return 1;
    picowal_fs_store_t fs;
    picowal_store_t store;
    if (!picowal_store_fs_open(&fs, root, &store)) return 1;
    picowal_api_set_store(&store);
    const char stored_jacket[] = "stored waterproof shell";
    const char stored_boots[] = "stored leather boots";
    if (picowal_api_put(3, 11, stored_jacket, sizeof(stored_jacket)) != PICOWAL_API_OK) return 1;
    if (picowal_api_put(3, 12, stored_boots, sizeof(stored_boots)) != PICOWAL_API_OK) return 1;
    picowal_search_init(&index);
    if (picowal_search_index_pack(&index, 3, extract_raw_text, NULL) != PICOWAL_SEARCH_OK) return 1;
    picowal_search_request_t stored_request = {
        .query_text = "waterproof",
        .limit = 1,
    };
    if (picowal_search_query(&index, &stored_request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 1 || !expect_key(&response, 0, 3, 11)) return 1;

    char segment_path[160];
    snprintf(segment_path, sizeof(segment_path), "%s/search.pwsi", root);
    if (picowal_search_save(&index, segment_path) != PICOWAL_SEARCH_OK) return 1;
    static picowal_search_index_t loaded;
    picowal_search_init(&loaded);
    if (picowal_search_load(&loaded, segment_path) != PICOWAL_SEARCH_OK) return 1;
    if (!picowal_search_is_compatible(&loaded, "retail-products", 7)) return 1;
    if (loaded.metadata.index_generation != 42) return 1;
    if (picowal_search_is_compatible(&loaded, "retail-products", 8)) return 1;
    if (picowal_search_query(&loaded, &stored_request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 1 || !expect_key(&response, 0, 3, 11)) return 1;
    if (loaded.doc_count != index.doc_count || loaded.term_count != index.term_count ||
        loaded.posting_count != index.posting_count ||
        loaded.facet_count != index.facet_count ||
        loaded.numeric_count != index.numeric_count ||
        loaded.vector_bucket_count != index.vector_bucket_count) {
        return 1;
    }

    char journal_path[160];
    snprintf(journal_path, sizeof(journal_path), "%s/search.pwsj", root);
    static picowal_search_index_t journaled;
    static picowal_search_index_t replayed;
    picowal_search_init(&journaled);
    if (picowal_search_journal_upsert(&journaled, journal_path, 4, 21, "journal waterproof jacket", jacket_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_journal_upsert(&journaled, journal_path, 4, 22, "journal leather boots", boots_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_journal_facet(&journaled, journal_path, 4, 21, "category", "outerwear") != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_journal_number(&journaled, journal_path, 4, 21, "price", 91.0f) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_journal_delete(&journaled, journal_path, 4, 22) != PICOWAL_SEARCH_OK) return 1;
    picowal_search_init(&replayed);
    if (picowal_search_journal_replay(&replayed, journal_path) != PICOWAL_SEARCH_OK) return 1;
    picowal_search_request_t journal_request = {
        .query_text = "waterproof",
        .limit = 2,
    };
    if (picowal_search_query(&replayed, &journal_request, &response) != PICOWAL_SEARCH_OK) return 1;
    if (response.hit_count != 1 || !expect_key(&response, 0, 4, 21)) return 1;
    if (picowal_search_facets(&replayed, "category", facet_counts, 4, &facet_count) != PICOWAL_SEARCH_OK) return 1;
    if (facet_count != 1 || strcmp(facet_counts[0].value, "outerwear") != 0 || facet_counts[0].count != 1) return 1;
    if (picowal_search_range(&replayed, "price", 90.0f, 92.0f, range_keys, 4, &range_count) != PICOWAL_SEARCH_OK) return 1;
    if (range_count != 1 || range_keys[0] != picowal_api_key(4, 21)) return 1;

    puts("picowal search smoke ok");
    char pack_dir[128];
    snprintf(pack_dir, sizeof(pack_dir), "%s/003", root);
    unlink(segment_path);
    unlink(journal_path);
    picowal_api_delete(3, 11);
    picowal_api_delete(3, 12);
    rmdir(pack_dir);
    rmdir(root);
    return 0;
}
