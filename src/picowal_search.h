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
#define PICOWAL_SEARCH_FIELD_MAX     31u
#define PICOWAL_SEARCH_FACET_VALUE_MAX 63u
#define PICOWAL_SEARCH_FACET_ENTRY_MAX 8192u
#define PICOWAL_SEARCH_NUMERIC_ENTRY_MAX 4096u
#define PICOWAL_SEARCH_FACET_RESULT_MAX 128u
#define PICOWAL_SEARCH_VECTOR_BUCKET_ENTRY_MAX PICOWAL_SEARCH_DOC_MAX
#define PICOWAL_SEARCH_NAME_MAX 63u

typedef enum {
    PICOWAL_SEARCH_OK = 0,
    PICOWAL_SEARCH_INVALID,
    PICOWAL_SEARCH_FULL,
    PICOWAL_SEARCH_NOT_FOUND,
    PICOWAL_SEARCH_IO,
    PICOWAL_SEARCH_CORRUPT,
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
    uint16_t doc_slot;
    char field[PICOWAL_SEARCH_FIELD_MAX + 1u];
    char value[PICOWAL_SEARCH_FACET_VALUE_MAX + 1u];
} picowal_search_facet_entry_t;

typedef struct {
    uint16_t doc_slot;
    char field[PICOWAL_SEARCH_FIELD_MAX + 1u];
    float value;
} picowal_search_numeric_entry_t;

typedef struct {
    uint16_t doc_slot;
    uint16_t bucket;
    uint64_t signature;
} picowal_search_vector_bucket_entry_t;

typedef struct {
    char name[PICOWAL_SEARCH_NAME_MAX + 1u];
    uint32_t schema_version;
    uint32_t index_generation;
    uint32_t flags;
} picowal_search_metadata_t;

typedef struct {
    picowal_search_metadata_t metadata;
    picowal_search_doc_t docs[PICOWAL_SEARCH_DOC_MAX];
    picowal_search_term_t terms[PICOWAL_SEARCH_TERM_MAX];
    picowal_search_posting_t postings[PICOWAL_SEARCH_POSTING_MAX];
    picowal_search_facet_entry_t facets[PICOWAL_SEARCH_FACET_ENTRY_MAX];
    picowal_search_numeric_entry_t numerics[PICOWAL_SEARCH_NUMERIC_ENTRY_MAX];
    picowal_search_vector_bucket_entry_t vector_buckets[PICOWAL_SEARCH_VECTOR_BUCKET_ENTRY_MAX];
    uint16_t doc_count;
    uint16_t term_count;
    uint32_t posting_count;
    uint32_t facet_count;
    uint32_t numeric_count;
    uint32_t vector_bucket_count;
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

typedef struct {
    char value[PICOWAL_SEARCH_FACET_VALUE_MAX + 1u];
    uint32_t count;
} picowal_search_facet_count_t;

typedef enum {
    PICOWAL_SEARCH_JOURNAL_UPSERT = 1,
    PICOWAL_SEARCH_JOURNAL_DELETE = 2,
    PICOWAL_SEARCH_JOURNAL_FACET = 3,
    PICOWAL_SEARCH_JOURNAL_NUMBER = 4,
} picowal_search_journal_op_t;

void picowal_search_init(picowal_search_index_t *index);
picowal_search_status_t picowal_search_configure(picowal_search_index_t *index,
                                                 const char *name,
                                                 uint32_t schema_version,
                                                 uint32_t index_generation,
                                                 uint32_t flags);
bool picowal_search_is_compatible(const picowal_search_index_t *index,
                                  const char *name,
                                  uint32_t schema_version);
picowal_search_status_t picowal_search_rebuild(picowal_search_index_t *index);
picowal_search_status_t picowal_search_upsert(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card,
                                              const char *text,
                                              const float *vector,
                                              uint16_t vector_dims);
picowal_search_status_t picowal_search_delete(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card);
picowal_search_status_t picowal_search_set_facet(picowal_search_index_t *index,
                                                 uint16_t pack,
                                                 uint32_t card,
                                                 const char *field,
                                                 const char *value);
picowal_search_status_t picowal_search_set_number(picowal_search_index_t *index,
                                                  uint16_t pack,
                                                  uint32_t card,
                                                  const char *field,
                                                  float value);
picowal_search_status_t picowal_search_clear_fields(picowal_search_index_t *index,
                                                    uint16_t pack,
                                                    uint32_t card);
picowal_search_status_t picowal_search_facets(const picowal_search_index_t *index,
                                              const char *field,
                                              picowal_search_facet_count_t *out,
                                              uint16_t max_out,
                                              uint16_t *out_count);
picowal_search_status_t picowal_search_range(const picowal_search_index_t *index,
                                             const char *field,
                                             float min_value,
                                             float max_value,
                                             uint32_t *out_keys,
                                             uint16_t max_keys,
                                             uint16_t *out_count);
picowal_search_status_t picowal_search_journal_upsert(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card,
                                                      const char *text,
                                                      const float *vector,
                                                      uint16_t vector_dims);
picowal_search_status_t picowal_search_journal_delete(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card);
picowal_search_status_t picowal_search_journal_facet(picowal_search_index_t *index,
                                                     const char *journal_path,
                                                     uint16_t pack,
                                                     uint32_t card,
                                                     const char *field,
                                                     const char *value);
picowal_search_status_t picowal_search_journal_number(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card,
                                                      const char *field,
                                                      float value);
picowal_search_status_t picowal_search_journal_replay(picowal_search_index_t *index,
                                                      const char *journal_path);
picowal_search_status_t picowal_search_index_pack(picowal_search_index_t *index,
                                                  uint16_t pack,
                                                  picowal_search_extract_fn extract,
                                                  void *ctx);
picowal_search_status_t picowal_search_save(const picowal_search_index_t *index,
                                            const char *path);
picowal_search_status_t picowal_search_load(picowal_search_index_t *index,
                                            const char *path);
picowal_search_status_t picowal_search_query(const picowal_search_index_t *index,
                                             const picowal_search_request_t *request,
                                             picowal_search_response_t *response);

#endif
