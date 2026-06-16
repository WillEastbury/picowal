#ifndef PICOWAL_SEARCH_H
#define PICOWAL_SEARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "picowal_api.h"

#define PICOWAL_SEARCH_DOC_MAX       1024u
#define PICOWAL_SEARCH_TERM_MAX      4096u
#define PICOWAL_SEARCH_POSTING_MAX   32768u
#define PICOWAL_SEARCH_TEXT_MAX      508u
#define PICOWAL_SEARCH_TOKEN_MAX     31u
#define PICOWAL_SEARCH_VECTOR_MAX    384u
#define PICOWAL_SEARCH_HIT_MAX       128u

typedef enum {
    PICOWAL_SEARCH_OK = 0,
    PICOWAL_SEARCH_INVALID,
    PICOWAL_SEARCH_FULL,
    PICOWAL_SEARCH_NOT_FOUND,
} picowal_search_status_t;

typedef struct {
    uint32_t key;
    bool active;
    uint16_t text_len;
    uint16_t doc_len;
    uint16_t vector_dims;
    uint64_t vector_sig;
    char text[PICOWAL_SEARCH_TEXT_MAX + 1u];
    float vector[PICOWAL_SEARCH_VECTOR_MAX];
} picowal_search_doc_t;

typedef struct {
    char token[PICOWAL_SEARCH_TOKEN_MAX + 1u];
    uint16_t doc_freq;
} picowal_search_term_t;

typedef struct {
    uint16_t term_id;
    uint16_t doc_slot;
    uint16_t term_freq;
} picowal_search_posting_t;

typedef struct {
    picowal_search_doc_t docs[PICOWAL_SEARCH_DOC_MAX];
    picowal_search_term_t terms[PICOWAL_SEARCH_TERM_MAX];
    picowal_search_posting_t postings[PICOWAL_SEARCH_POSTING_MAX];
    uint16_t doc_count;
    uint16_t term_count;
    uint32_t posting_count;
    uint32_t total_doc_len;
    bool overflow;
} picowal_search_index_t;

typedef float (*picowal_semantic_score_fn)(const char *query,
                                           const char *document,
                                           void *ctx);

typedef bool (*picowal_search_extract_fn)(uint32_t key,
                                          const uint8_t *value,
                                          uint16_t value_len,
                                          char *out_text,
                                          uint16_t out_text_cap,
                                          float *out_vector,
                                          uint16_t *inout_vector_dims,
                                          void *ctx);

typedef struct {
    const char *query_text;
    const float *query_vector;
    uint16_t query_vector_dims;
    uint16_t limit;
    uint16_t candidate_limit;
    float lexical_weight;
    float vector_weight;
    float semantic_weight;
    picowal_semantic_score_fn semantic_score;
    void *semantic_ctx;
} picowal_search_request_t;

typedef struct {
    uint32_t key;
    float score;
    float bm25_score;
    float vector_score;
    float semantic_score;
    float rrf_score;
    uint16_t lexical_rank;
    uint16_t vector_rank;
} picowal_search_hit_t;

typedef struct {
    uint16_t lexical_candidates;
    uint16_t vector_candidates;
    uint16_t hybrid_candidates;
    uint16_t semantic_reranked;
    bool used_vector_ann;
} picowal_query_plan_t;

typedef struct {
    picowal_search_hit_t hits[PICOWAL_SEARCH_HIT_MAX];
    uint16_t hit_count;
    picowal_query_plan_t plan;
} picowal_search_response_t;

void picowal_search_init(picowal_search_index_t *index);
picowal_search_status_t picowal_search_upsert(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card,
                                              const char *text,
                                              const float *vector,
                                              uint16_t vector_dims);
picowal_search_status_t picowal_search_delete(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card);
picowal_search_status_t picowal_search_index_pack(picowal_search_index_t *index,
                                                  uint16_t pack,
                                                  picowal_search_extract_fn extract,
                                                  void *ctx);
picowal_search_status_t picowal_search_query(const picowal_search_index_t *index,
                                             const picowal_search_request_t *request,
                                             picowal_search_response_t *response);

#endif
