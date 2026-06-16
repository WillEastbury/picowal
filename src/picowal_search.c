#include "picowal_search.h"

#if !defined(PICOWAL_HOST)
#error "picowal_search is a host/server-side primitive; do not compile it into Pico firmware."
#endif

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t doc_slot;
    float score;
} scored_doc_t;

typedef struct {
    char token[PICOWAL_SEARCH_TOKEN_MAX + 1u];
    uint16_t count;
} token_count_t;

static uint16_t text_copy(char *dst, const char *src) {
    uint16_t n = 0;
    if (!dst || !src) return 0;
    while (src[n] && n < PICOWAL_SEARCH_TEXT_MAX) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

static uint16_t next_token(const char *text, uint16_t *offset, char *out) {
    uint16_t pos = *offset;
    uint16_t len = 0;
    while (text[pos] && !isalnum((unsigned char)text[pos])) pos++;
    while (text[pos] && isalnum((unsigned char)text[pos]) && len < PICOWAL_SEARCH_TOKEN_MAX) {
        out[len++] = (char)tolower((unsigned char)text[pos++]);
    }
    while (text[pos] && isalnum((unsigned char)text[pos])) pos++;
    out[len] = 0;
    *offset = pos;
    return len;
}

static uint16_t collect_counts(const char *text, token_count_t *counts, uint16_t max_counts) {
    uint16_t offset = 0;
    uint16_t used = 0;
    char token[PICOWAL_SEARCH_TOKEN_MAX + 1u];
    while (next_token(text, &offset, token) > 0) {
        uint16_t i;
        for (i = 0; i < used; i++) {
            if (strcmp(counts[i].token, token) == 0) {
                counts[i].count++;
                break;
            }
        }
        if (i == used && used < max_counts) {
            memcpy(counts[used].token, token, sizeof(counts[used].token));
            counts[used].count = 1;
            used++;
        }
    }
    return used;
}

static uint64_t vector_signature(const float *vector, uint16_t dims) {
    uint64_t sig = 0;
    uint16_t n = dims < 64u ? dims : 64u;
    for (uint16_t i = 0; i < n; i++) {
        if (vector[i] >= 0.0f) sig |= (1ull << i);
    }
    return sig;
}

static float cosine_score(const float *a, const float *b, uint16_t dims) {
    float dot = 0.0f;
    float an = 0.0f;
    float bn = 0.0f;
    for (uint16_t i = 0; i < dims; i++) {
        dot += a[i] * b[i];
        an += a[i] * a[i];
        bn += b[i] * b[i];
    }
    if (an <= 0.0f || bn <= 0.0f) return 0.0f;
    return dot / sqrtf(an * bn);
}

static uint16_t popcount64(uint64_t value) {
    uint16_t count = 0;
    while (value) {
        value &= value - 1u;
        count++;
    }
    return count;
}

static int score_desc(const void *left, const void *right) {
    const scored_doc_t *a = (const scored_doc_t *)left;
    const scored_doc_t *b = (const scored_doc_t *)right;
    if (a->score < b->score) return 1;
    if (a->score > b->score) return -1;
    if (a->doc_slot > b->doc_slot) return 1;
    if (a->doc_slot < b->doc_slot) return -1;
    return 0;
}

static int hit_desc(const void *left, const void *right) {
    const picowal_search_hit_t *a = (const picowal_search_hit_t *)left;
    const picowal_search_hit_t *b = (const picowal_search_hit_t *)right;
    if (a->score < b->score) return 1;
    if (a->score > b->score) return -1;
    if (a->key > b->key) return 1;
    if (a->key < b->key) return -1;
    return 0;
}

static int hamming_asc(const void *left, const void *right) {
    const scored_doc_t *a = (const scored_doc_t *)left;
    const scored_doc_t *b = (const scored_doc_t *)right;
    if (a->score > b->score) return 1;
    if (a->score < b->score) return -1;
    if (a->doc_slot > b->doc_slot) return 1;
    if (a->doc_slot < b->doc_slot) return -1;
    return 0;
}

static int find_doc_slot(const picowal_search_index_t *index, uint32_t key) {
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (index->docs[i].active && index->docs[i].key == key) return (int)i;
    }
    return -1;
}

static int find_term(const picowal_search_index_t *index, const char *token) {
    for (uint16_t i = 0; i < index->term_count; i++) {
        if (strcmp(index->terms[i].token, token) == 0) return (int)i;
    }
    return -1;
}

static int ensure_term(picowal_search_index_t *index, const char *token) {
    int existing = find_term(index, token);
    if (existing >= 0) return existing;
    if (index->term_count >= PICOWAL_SEARCH_TERM_MAX) {
        index->overflow = true;
        return -1;
    }
    uint16_t id = index->term_count++;
    memcpy(index->terms[id].token, token, PICOWAL_SEARCH_TOKEN_MAX + 1u);
    index->terms[id].doc_freq = 0;
    return (int)id;
}

static bool rebuild(picowal_search_index_t *index) {
    index->term_count = 0;
    index->posting_count = 0;
    index->total_doc_len = 0;
    index->overflow = false;

    token_count_t counts[128];
    for (uint16_t doc_slot = 0; doc_slot < index->doc_count; doc_slot++) {
        picowal_search_doc_t *doc = &index->docs[doc_slot];
        if (!doc->active) continue;
        memset(counts, 0, sizeof(counts));
        uint16_t unique = collect_counts(doc->text, counts, 128);
        uint16_t doc_len = 0;
        for (uint16_t i = 0; i < unique; i++) doc_len = (uint16_t)(doc_len + counts[i].count);
        doc->doc_len = doc_len;
        index->total_doc_len += doc_len;
        for (uint16_t i = 0; i < unique; i++) {
            int term_id = ensure_term(index, counts[i].token);
            if (term_id < 0 || index->posting_count >= PICOWAL_SEARCH_POSTING_MAX) {
                index->overflow = true;
                return false;
            }
            index->terms[term_id].doc_freq++;
            index->postings[index->posting_count++] = (picowal_search_posting_t){
                .term_id = (uint16_t)term_id,
                .doc_slot = doc_slot,
                .term_freq = counts[i].count,
            };
        }
    }
    return true;
}

void picowal_search_init(picowal_search_index_t *index) {
    if (!index) return;
    memset(index, 0, sizeof(*index));
}

picowal_search_status_t picowal_search_upsert(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card,
                                              const char *text,
                                              const float *vector,
                                              uint16_t vector_dims) {
    if (!index || !text || pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX ||
        vector_dims > PICOWAL_SEARCH_VECTOR_MAX || (vector_dims > 0 && !vector)) {
        return PICOWAL_SEARCH_INVALID;
    }

    uint32_t key = picowal_api_key(pack, card);
    int slot = find_doc_slot(index, key);
    if (slot < 0) {
        if (index->doc_count >= PICOWAL_SEARCH_DOC_MAX) return PICOWAL_SEARCH_FULL;
        slot = index->doc_count++;
    }

    picowal_search_doc_t *doc = &index->docs[slot];
    memset(doc, 0, sizeof(*doc));
    doc->key = key;
    doc->active = true;
    doc->text_len = text_copy(doc->text, text);
    doc->vector_dims = vector_dims;
    for (uint16_t i = 0; i < vector_dims; i++) doc->vector[i] = vector[i];
    doc->vector_sig = vector_dims ? vector_signature(vector, vector_dims) : 0;

    return rebuild(index) ? PICOWAL_SEARCH_OK : PICOWAL_SEARCH_FULL;
}

picowal_search_status_t picowal_search_delete(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card) {
    if (!index || pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX) {
        return PICOWAL_SEARCH_INVALID;
    }
    int slot = find_doc_slot(index, picowal_api_key(pack, card));
    if (slot < 0) return PICOWAL_SEARCH_NOT_FOUND;
    index->docs[slot].active = false;
    return rebuild(index) ? PICOWAL_SEARCH_OK : PICOWAL_SEARCH_FULL;
}

picowal_search_status_t picowal_search_index_pack(picowal_search_index_t *index,
                                                  uint16_t pack,
                                                  picowal_search_extract_fn extract,
                                                  void *ctx) {
    if (!index || !extract || pack < 2 || pack > PICOWAL_PACK_MAX) return PICOWAL_SEARCH_INVALID;
    uint32_t cards[PICOWAL_SEARCH_DOC_MAX];
    uint32_t count = picowal_api_list(pack, cards, PICOWAL_SEARCH_DOC_MAX);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t value[PICOWAL_VALUE_MAX];
        uint16_t value_len = sizeof(value);
        picowal_api_status_t st = picowal_api_get(pack, cards[i], value, sizeof(value), &value_len);
        if (st != PICOWAL_API_OK) return PICOWAL_SEARCH_INVALID;

        char text[PICOWAL_SEARCH_TEXT_MAX + 1u];
        float vector[PICOWAL_SEARCH_VECTOR_MAX];
        uint16_t vector_dims = PICOWAL_SEARCH_VECTOR_MAX;
        memset(text, 0, sizeof(text));
        memset(vector, 0, sizeof(vector));
        if (!extract(picowal_api_key(pack, cards[i]), value, value_len, text, sizeof(text),
                     vector, &vector_dims, ctx)) {
            continue;
        }
        picowal_search_status_t upsert = picowal_search_upsert(index, pack, cards[i], text, vector, vector_dims);
        if (upsert != PICOWAL_SEARCH_OK) return upsert;
    }
    return PICOWAL_SEARCH_OK;
}

static void bm25_scores(const picowal_search_index_t *index, const char *query,
                        float *scores, scored_doc_t *ranked, uint16_t *ranked_count) {
    token_count_t qcounts[64];
    memset(qcounts, 0, sizeof(qcounts));
    uint16_t qn = collect_counts(query ? query : "", qcounts, 64);
    float avgdl = index->doc_count ? (float)index->total_doc_len / (float)index->doc_count : 0.0f;
    if (avgdl <= 0.0f) avgdl = 1.0f;
    const float k1 = 1.2f;
    const float b = 0.75f;

    for (uint16_t qi = 0; qi < qn; qi++) {
        int term_id = find_term(index, qcounts[qi].token);
        if (term_id < 0) continue;
        uint16_t df = index->terms[term_id].doc_freq;
        float idf = logf(((float)index->doc_count - (float)df + 0.5f) / ((float)df + 0.5f) + 1.0f);
        for (uint32_t pi = 0; pi < index->posting_count; pi++) {
            const picowal_search_posting_t *posting = &index->postings[pi];
            if (posting->term_id != (uint16_t)term_id) continue;
            const picowal_search_doc_t *doc = &index->docs[posting->doc_slot];
            float tf = (float)posting->term_freq;
            float denom = tf + k1 * (1.0f - b + b * ((float)doc->doc_len / avgdl));
            scores[posting->doc_slot] += idf * ((tf * (k1 + 1.0f)) / denom);
        }
    }

    *ranked_count = 0;
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (!index->docs[i].active || scores[i] <= 0.0f) continue;
        ranked[*ranked_count] = (scored_doc_t){.doc_slot = i, .score = scores[i]};
        (*ranked_count)++;
    }
    qsort(ranked, *ranked_count, sizeof(ranked[0]), score_desc);
}

static void vector_ann_scores(const picowal_search_index_t *index,
                              const float *query_vector,
                              uint16_t dims,
                              uint16_t candidate_limit,
                              float *scores,
                              scored_doc_t *ranked,
                              uint16_t *ranked_count) {
    scored_doc_t hamming[PICOWAL_SEARCH_DOC_MAX];
    uint16_t hcount = 0;
    uint64_t qsig = vector_signature(query_vector, dims);
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (!index->docs[i].active || index->docs[i].vector_dims != dims) continue;
        hamming[hcount++] = (scored_doc_t){
            .doc_slot = i,
            .score = (float)popcount64(qsig ^ index->docs[i].vector_sig),
        };
    }
    qsort(hamming, hcount, sizeof(hamming[0]), hamming_asc);
    if (candidate_limit == 0 || candidate_limit > hcount) candidate_limit = hcount;

    *ranked_count = 0;
    for (uint16_t i = 0; i < candidate_limit; i++) {
        uint16_t slot = hamming[i].doc_slot;
        float score = cosine_score(query_vector, index->docs[slot].vector, dims);
        if (score <= 0.0f) continue;
        scores[slot] = score;
        ranked[*ranked_count] = (scored_doc_t){.doc_slot = slot, .score = score};
        (*ranked_count)++;
    }
    qsort(ranked, *ranked_count, sizeof(ranked[0]), score_desc);
}

static picowal_search_hit_t *ensure_hit(picowal_search_response_t *response, uint32_t key) {
    for (uint16_t i = 0; i < response->hit_count; i++) {
        if (response->hits[i].key == key) return &response->hits[i];
    }
    if (response->hit_count >= PICOWAL_SEARCH_HIT_MAX) return NULL;
    picowal_search_hit_t *hit = &response->hits[response->hit_count++];
    memset(hit, 0, sizeof(*hit));
    hit->key = key;
    return hit;
}

picowal_search_status_t picowal_search_query(const picowal_search_index_t *index,
                                             const picowal_search_request_t *request,
                                             picowal_search_response_t *response) {
    if (!index || !request || !response) return PICOWAL_SEARCH_INVALID;
    if (request->query_vector_dims > PICOWAL_SEARCH_VECTOR_MAX ||
        (request->query_vector_dims > 0 && !request->query_vector)) {
        return PICOWAL_SEARCH_INVALID;
    }

    memset(response, 0, sizeof(*response));
    uint16_t limit = request->limit;
    if (limit == 0 || limit > PICOWAL_SEARCH_HIT_MAX) limit = PICOWAL_SEARCH_HIT_MAX;
    uint16_t candidate_limit = request->candidate_limit;
    if (candidate_limit == 0 || candidate_limit > PICOWAL_SEARCH_HIT_MAX) candidate_limit = PICOWAL_SEARCH_HIT_MAX;

    float bm25[PICOWAL_SEARCH_DOC_MAX] = {0};
    float vector[PICOWAL_SEARCH_DOC_MAX] = {0};
    scored_doc_t bm25_ranked[PICOWAL_SEARCH_DOC_MAX];
    scored_doc_t vector_ranked[PICOWAL_SEARCH_DOC_MAX];
    uint16_t bm25_count = 0;
    uint16_t vector_count = 0;

    bm25_scores(index, request->query_text, bm25, bm25_ranked, &bm25_count);
    response->plan.lexical_candidates = bm25_count;

    if (request->query_vector_dims > 0) {
        vector_ann_scores(index, request->query_vector, request->query_vector_dims, candidate_limit,
                          vector, vector_ranked, &vector_count);
        response->plan.vector_candidates = vector_count;
        response->plan.used_vector_ann = true;
    }

    const float rrf_k = 60.0f;
    for (uint16_t i = 0; i < bm25_count && i < candidate_limit; i++) {
        uint16_t slot = bm25_ranked[i].doc_slot;
        picowal_search_hit_t *hit = ensure_hit(response, index->docs[slot].key);
        if (!hit) continue;
        hit->bm25_score = bm25[slot];
        hit->lexical_rank = (uint16_t)(i + 1u);
        hit->rrf_score += 1.0f / (rrf_k + (float)i + 1.0f);
    }
    for (uint16_t i = 0; i < vector_count && i < candidate_limit; i++) {
        uint16_t slot = vector_ranked[i].doc_slot;
        picowal_search_hit_t *hit = ensure_hit(response, index->docs[slot].key);
        if (!hit) continue;
        hit->vector_score = vector[slot];
        hit->vector_rank = (uint16_t)(i + 1u);
        hit->rrf_score += 1.0f / (rrf_k + (float)i + 1.0f);
    }
    response->plan.hybrid_candidates = response->hit_count;

    float lexical_weight = request->lexical_weight > 0.0f ? request->lexical_weight : 1.0f;
    float vector_weight = request->vector_weight > 0.0f ? request->vector_weight : 1.0f;
    float semantic_weight = request->semantic_weight > 0.0f ? request->semantic_weight : 1.0f;
    for (uint16_t i = 0; i < response->hit_count; i++) {
        picowal_search_hit_t *hit = &response->hits[i];
        hit->score = hit->rrf_score + lexical_weight * hit->bm25_score + vector_weight * hit->vector_score;
        if (request->semantic_score && request->query_text) {
            int slot = find_doc_slot(index, hit->key);
            if (slot >= 0) {
                hit->semantic_score = request->semantic_score(request->query_text, index->docs[slot].text,
                                                              request->semantic_ctx);
                hit->score += semantic_weight * hit->semantic_score;
                response->plan.semantic_reranked++;
            }
        }
    }

    qsort(response->hits, response->hit_count, sizeof(response->hits[0]), hit_desc);
    if (response->hit_count > limit) response->hit_count = limit;
    return PICOWAL_SEARCH_OK;
}
