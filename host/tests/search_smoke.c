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
    picowal_search_index_t index;
    picowal_search_init(&index);

    const float jacket_vec[] = {1.0f, 0.0f, 0.1f, 0.0f};
    const float boots_vec[] = {0.0f, 1.0f, 0.0f, 0.1f};
    const float shell_vec[] = {0.95f, 0.0f, 0.2f, 0.0f};
    const float query_vec[] = {1.0f, 0.0f, 0.0f, 0.0f};

    if (picowal_search_upsert(&index, 2, 1, "blue waterproof jacket rain hiking", jacket_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_upsert(&index, 2, 2, "leather city boots smart casual", boots_vec, 4) != PICOWAL_SEARCH_OK) return 1;
    if (picowal_search_upsert(&index, 2, 3, "waterproof hiking shell premium trail", shell_vec, 4) != PICOWAL_SEARCH_OK) return 1;

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

    if (picowal_search_delete(&index, 2, 3) != PICOWAL_SEARCH_OK) return 1;
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

    puts("picowal search smoke ok");
    char pack_dir[128];
    snprintf(pack_dir, sizeof(pack_dir), "%s/003", root);
    picowal_api_delete(3, 11);
    picowal_api_delete(3, 12);
    rmdir(pack_dir);
    rmdir(root);
    return 0;
}
