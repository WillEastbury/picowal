#include "picowal_search.h"

#if !defined(PICOWAL_HOST)
#error "picowal_search is a host/server-side primitive; do not compile it into Pico firmware."
#endif

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PICOWAL_SEARCH_SEG_MAGIC   0x50575349u /* "PWSI" */
#define PICOWAL_SEARCH_SEG_VERSION 1u
#define PICOWAL_SEARCH_JOURNAL_MAGIC   0x5057534Au /* "PWSJ" */
#define PICOWAL_SEARCH_JOURNAL_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t doc_max;
    uint16_t term_max;
    uint32_t posting_max;
    uint16_t text_max;
    uint16_t vector_max;
    uint16_t token_max;
    uint16_t doc_count;
    uint16_t term_count;
    uint16_t flags;
    uint32_t posting_count;
    uint32_t facet_count;
    uint32_t numeric_count;
    uint32_t vector_bucket_count;
    uint32_t total_doc_len;
    uint32_t docs_crc32;
    uint32_t terms_crc32;
    uint32_t postings_crc32;
    uint32_t facets_crc32;
    uint32_t numerics_crc32;
    uint32_t vector_buckets_crc32;
    uint32_t schema_version;
    uint32_t index_generation;
    uint32_t metadata_flags;
    char name[PICOWAL_SEARCH_NAME_MAX + 1u];
} picowal_search_segment_header_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t op;
    uint16_t pack;
    uint16_t text_len;
    uint32_t card;
    uint16_t vector_dims;
    uint16_t field_len;
    uint16_t value_len;
    uint16_t reserved;
    float numeric_value;
    uint32_t payload_len;
    uint32_t payload_crc32;
} picowal_search_journal_header_t;

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

static uint16_t bounded_copy(char *dst, uint16_t cap, const char *src) {
    uint16_t n = 0;
    if (!dst || cap == 0 || !src || !*src) return 0;
    while (src[n] && n + 1u < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

static uint16_t bounded_len(const char *src, uint16_t max) {
    uint16_t n = 0;
    if (!src) return 0;
    while (src[n] && n < max) n++;
    return n;
}

static uint32_t search_crc32(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool write_exact(FILE *file, const void *data, size_t size) {
    return fwrite(data, 1, size, file) == size;
}

static bool read_exact(FILE *file, void *data, size_t size) {
    return fread(data, 1, size, file) == size;
}

static uint32_t journal_payload_len(uint16_t text_len, uint16_t vector_dims,
                                    uint16_t field_len, uint16_t value_len) {
    return (uint32_t)text_len + ((uint32_t)vector_dims * sizeof(float)) +
           (uint32_t)field_len + (uint32_t)value_len;
}

static picowal_search_status_t append_journal_record(const char *journal_path,
                                                     uint16_t op,
                                                     uint16_t pack,
                                                     uint32_t card,
                                                     const char *text,
                                                     const float *vector,
                                                     uint16_t vector_dims,
                                                     const char *field,
                                                     const char *value,
                                                     float numeric_value) {
    if (!journal_path || !*journal_path) return PICOWAL_SEARCH_INVALID;
    uint16_t text_len = bounded_len(text, PICOWAL_SEARCH_TEXT_MAX);
    uint16_t field_len = bounded_len(field, PICOWAL_SEARCH_FIELD_MAX);
    uint16_t value_len = bounded_len(value, PICOWAL_SEARCH_FACET_VALUE_MAX);
    if (vector_dims > PICOWAL_SEARCH_VECTOR_MAX || (vector_dims > 0 && !vector)) return PICOWAL_SEARCH_INVALID;
    uint32_t payload_len = journal_payload_len(text_len, vector_dims, field_len, value_len);
    uint8_t payload[PICOWAL_SEARCH_TEXT_MAX + (PICOWAL_SEARCH_VECTOR_MAX * sizeof(float)) +
                    PICOWAL_SEARCH_FIELD_MAX + PICOWAL_SEARCH_FACET_VALUE_MAX];
    uint32_t off = 0;
    if (text_len) {
        memcpy(payload + off, text, text_len);
        off += text_len;
    }
    if (vector_dims) {
        memcpy(payload + off, vector, (size_t)vector_dims * sizeof(float));
        off += (uint32_t)vector_dims * sizeof(float);
    }
    if (field_len) {
        memcpy(payload + off, field, field_len);
        off += field_len;
    }
    if (value_len) {
        memcpy(payload + off, value, value_len);
        off += value_len;
    }
    if (off != payload_len) return PICOWAL_SEARCH_INVALID;

    picowal_search_journal_header_t header = {
        .magic = PICOWAL_SEARCH_JOURNAL_MAGIC,
        .version = PICOWAL_SEARCH_JOURNAL_VERSION,
        .op = op,
        .pack = pack,
        .text_len = text_len,
        .card = card,
        .vector_dims = vector_dims,
        .field_len = field_len,
        .value_len = value_len,
        .reserved = 0,
        .numeric_value = numeric_value,
        .payload_len = payload_len,
        .payload_crc32 = search_crc32(payload, payload_len),
    };
    FILE *file = fopen(journal_path, "ab");
    if (!file) return PICOWAL_SEARCH_IO;
    bool ok = write_exact(file, &header, sizeof(header)) && write_exact(file, payload, payload_len);
    if (fclose(file) != 0) ok = false;
    return ok ? PICOWAL_SEARCH_OK : PICOWAL_SEARCH_IO;
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

static uint16_t vector_bucket(uint64_t signature) {
    return (uint16_t)(signature & 0x00FFu);
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

static int find_doc_slot_any(const picowal_search_index_t *index, uint32_t key) {
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (index->docs[i].key == key) return (int)i;
    }
    return -1;
}

static int find_free_doc_slot(const picowal_search_index_t *index) {
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (!index->docs[i].active) return (int)i;
    }
    return -1;
}

static uint16_t active_doc_count(const picowal_search_index_t *index) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < index->doc_count; i++) {
        if (index->docs[i].active) count++;
    }
    return count;
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

static void remove_doc_vector_bucket(picowal_search_index_t *index, uint16_t doc_slot);
static bool add_doc_vector_bucket(picowal_search_index_t *index, uint16_t doc_slot);

picowal_search_status_t picowal_search_rebuild(picowal_search_index_t *index) {
    if (!index) return PICOWAL_SEARCH_INVALID;
    index->term_count = 0;
    index->posting_count = 0;
    index->vector_bucket_count = 0;
    index->total_doc_len = 0;
    index->overflow = false;

    token_count_t counts[128];
    for (uint16_t doc_slot = 0; doc_slot < index->doc_count; doc_slot++) {
        picowal_search_doc_t *doc = &index->docs[doc_slot];
        if (!doc->active) continue;
        if (!add_doc_vector_bucket(index, doc_slot)) return PICOWAL_SEARCH_FULL;
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
                return PICOWAL_SEARCH_FULL;
            }
            index->terms[term_id].doc_freq++;
            index->postings[index->posting_count++] = (picowal_search_posting_t){
                .term_id = (uint16_t)term_id,
                .doc_slot = doc_slot,
                .term_freq = counts[i].count,
            };
        }
    }
    return PICOWAL_SEARCH_OK;
}

static bool can_add_postings(const picowal_search_index_t *index,
                             const token_count_t *counts,
                             uint16_t unique) {
    uint16_t new_terms = 0;
    for (uint16_t i = 0; i < unique; i++) {
        if (find_term(index, counts[i].token) < 0) new_terms++;
    }
    return (uint32_t)index->posting_count + unique <= PICOWAL_SEARCH_POSTING_MAX &&
           (uint32_t)index->term_count + new_terms <= PICOWAL_SEARCH_TERM_MAX;
}

static bool can_upsert_doc(const picowal_search_index_t *index,
                           int existing_slot,
                           const char *text) {
    token_count_t counts[128];
    memset(counts, 0, sizeof(counts));
    uint16_t unique = collect_counts(text, counts, 128);
    uint16_t new_terms = 0;
    for (uint16_t i = 0; i < unique; i++) {
        if (find_term(index, counts[i].token) < 0) new_terms++;
    }

    uint32_t old_postings = 0;
    if (existing_slot >= 0 && index->docs[existing_slot].active) {
        for (uint32_t i = 0; i < index->posting_count; i++) {
            if (index->postings[i].doc_slot == (uint16_t)existing_slot) old_postings++;
        }
    }
    uint32_t next_postings = index->posting_count - old_postings + unique;
    return next_postings <= PICOWAL_SEARCH_POSTING_MAX &&
           (uint32_t)index->term_count + new_terms <= PICOWAL_SEARCH_TERM_MAX;
}

static bool add_doc_postings(picowal_search_index_t *index, uint16_t doc_slot) {
    token_count_t counts[128];
    memset(counts, 0, sizeof(counts));
    picowal_search_doc_t *doc = &index->docs[doc_slot];
    uint16_t unique = collect_counts(doc->text, counts, 128);
    if (!can_add_postings(index, counts, unique)) {
        index->overflow = true;
        return false;
    }

    uint16_t doc_len = 0;
    for (uint16_t i = 0; i < unique; i++) doc_len = (uint16_t)(doc_len + counts[i].count);
    doc->doc_len = doc_len;
    index->total_doc_len += doc_len;

    for (uint16_t i = 0; i < unique; i++) {
        int term_id = ensure_term(index, counts[i].token);
        if (term_id < 0) return false;
        index->terms[term_id].doc_freq++;
        index->postings[index->posting_count++] = (picowal_search_posting_t){
            .term_id = (uint16_t)term_id,
            .doc_slot = doc_slot,
            .term_freq = counts[i].count,
        };
    }
    return true;
}

static void remove_doc_postings(picowal_search_index_t *index, uint16_t doc_slot) {
    if (index->docs[doc_slot].doc_len <= index->total_doc_len) {
        index->total_doc_len -= index->docs[doc_slot].doc_len;
    } else {
        index->total_doc_len = 0;
    }

    uint32_t write = 0;
    for (uint32_t read = 0; read < index->posting_count; read++) {
        picowal_search_posting_t posting = index->postings[read];
        if (posting.doc_slot == doc_slot) {
            if (posting.term_id < index->term_count && index->terms[posting.term_id].doc_freq > 0) {
                index->terms[posting.term_id].doc_freq--;
            }
            continue;
        }
        index->postings[write++] = posting;
    }
    index->posting_count = write;
}

static void remove_doc_facets(picowal_search_index_t *index, uint16_t doc_slot) {
    uint32_t write = 0;
    for (uint32_t read = 0; read < index->facet_count; read++) {
        picowal_search_facet_entry_t entry = index->facets[read];
        if (entry.doc_slot == doc_slot) continue;
        index->facets[write++] = entry;
    }
    index->facet_count = write;
}

static void remove_doc_numerics(picowal_search_index_t *index, uint16_t doc_slot) {
    uint32_t write = 0;
    for (uint32_t read = 0; read < index->numeric_count; read++) {
        picowal_search_numeric_entry_t entry = index->numerics[read];
        if (entry.doc_slot == doc_slot) continue;
        index->numerics[write++] = entry;
    }
    index->numeric_count = write;
}

static void remove_doc_vector_bucket(picowal_search_index_t *index, uint16_t doc_slot) {
    uint32_t write = 0;
    for (uint32_t read = 0; read < index->vector_bucket_count; read++) {
        picowal_search_vector_bucket_entry_t entry = index->vector_buckets[read];
        if (entry.doc_slot == doc_slot) continue;
        index->vector_buckets[write++] = entry;
    }
    index->vector_bucket_count = write;
}

static bool add_doc_vector_bucket(picowal_search_index_t *index, uint16_t doc_slot) {
    const picowal_search_doc_t *doc = &index->docs[doc_slot];
    if (doc->vector_dims == 0) return true;
    if (index->vector_bucket_count >= PICOWAL_SEARCH_VECTOR_BUCKET_ENTRY_MAX) {
        index->overflow = true;
        return false;
    }
    index->vector_buckets[index->vector_bucket_count++] = (picowal_search_vector_bucket_entry_t){
        .doc_slot = doc_slot,
        .bucket = vector_bucket(doc->vector_sig),
        .signature = doc->vector_sig,
    };
    return true;
}

void picowal_search_init(picowal_search_index_t *index) {
    if (!index) return;
    memset(index, 0, sizeof(*index));
    bounded_copy(index->metadata.name, sizeof(index->metadata.name), "default");
    index->metadata.schema_version = 1;
    index->metadata.index_generation = 1;
}

picowal_search_status_t picowal_search_configure(picowal_search_index_t *index,
                                                 const char *name,
                                                 uint32_t schema_version,
                                                 uint32_t index_generation,
                                                 uint32_t flags) {
    if (!index || !name || !*name || schema_version == 0 || index_generation == 0) {
        return PICOWAL_SEARCH_INVALID;
    }
    memset(&index->metadata, 0, sizeof(index->metadata));
    if (!bounded_copy(index->metadata.name, sizeof(index->metadata.name), name)) {
        return PICOWAL_SEARCH_INVALID;
    }
    index->metadata.schema_version = schema_version;
    index->metadata.index_generation = index_generation;
    index->metadata.flags = flags;
    return PICOWAL_SEARCH_OK;
}

bool picowal_search_is_compatible(const picowal_search_index_t *index,
                                  const char *name,
                                  uint32_t schema_version) {
    return index && name && *name &&
           strcmp(index->metadata.name, name) == 0 &&
           index->metadata.schema_version == schema_version;
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
    int slot = find_doc_slot_any(index, key);
    if (!can_upsert_doc(index, slot, text)) {
        index->overflow = true;
        return PICOWAL_SEARCH_FULL;
    }
    if (slot < 0) {
        slot = find_free_doc_slot(index);
        if (slot < 0) {
            if (index->doc_count >= PICOWAL_SEARCH_DOC_MAX) return PICOWAL_SEARCH_FULL;
            slot = index->doc_count++;
        }
    } else if (index->docs[slot].active) {
        remove_doc_postings(index, (uint16_t)slot);
        remove_doc_vector_bucket(index, (uint16_t)slot);
    }

    picowal_search_doc_t *doc = &index->docs[slot];
    memset(doc, 0, sizeof(*doc));
    doc->key = key;
    doc->active = true;
    doc->text_len = text_copy(doc->text, text);
    doc->vector_dims = vector_dims;
    for (uint16_t i = 0; i < vector_dims; i++) doc->vector[i] = vector[i];
    doc->vector_sig = vector_dims ? vector_signature(vector, vector_dims) : 0;

    return add_doc_postings(index, (uint16_t)slot) && add_doc_vector_bucket(index, (uint16_t)slot)
        ? PICOWAL_SEARCH_OK
        : PICOWAL_SEARCH_FULL;
}

picowal_search_status_t picowal_search_delete(picowal_search_index_t *index,
                                              uint16_t pack,
                                              uint32_t card) {
    if (!index || pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX) {
        return PICOWAL_SEARCH_INVALID;
    }
    int slot = find_doc_slot(index, picowal_api_key(pack, card));
    if (slot < 0) return PICOWAL_SEARCH_NOT_FOUND;
    remove_doc_postings(index, (uint16_t)slot);
    remove_doc_facets(index, (uint16_t)slot);
    remove_doc_numerics(index, (uint16_t)slot);
    remove_doc_vector_bucket(index, (uint16_t)slot);
    memset(&index->docs[slot], 0, sizeof(index->docs[slot]));
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_set_facet(picowal_search_index_t *index,
                                                 uint16_t pack,
                                                 uint32_t card,
                                                 const char *field,
                                                 const char *value) {
    if (!index || !field || !*field || !value || !*value ||
        pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX) {
        return PICOWAL_SEARCH_INVALID;
    }
    int slot = find_doc_slot(index, picowal_api_key(pack, card));
    if (slot < 0) return PICOWAL_SEARCH_NOT_FOUND;
    for (uint32_t i = 0; i < index->facet_count; i++) {
        picowal_search_facet_entry_t *entry = &index->facets[i];
        if (entry->doc_slot == (uint16_t)slot &&
            strcmp(entry->field, field) == 0 &&
            strcmp(entry->value, value) == 0) {
            return PICOWAL_SEARCH_OK;
        }
    }
    if (index->facet_count >= PICOWAL_SEARCH_FACET_ENTRY_MAX) {
        index->overflow = true;
        return PICOWAL_SEARCH_FULL;
    }
    picowal_search_facet_entry_t *entry = &index->facets[index->facet_count++];
    memset(entry, 0, sizeof(*entry));
    entry->doc_slot = (uint16_t)slot;
    if (!bounded_copy(entry->field, sizeof(entry->field), field) ||
        !bounded_copy(entry->value, sizeof(entry->value), value)) {
        index->facet_count--;
        return PICOWAL_SEARCH_INVALID;
    }
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_set_number(picowal_search_index_t *index,
                                                  uint16_t pack,
                                                  uint32_t card,
                                                  const char *field,
                                                  float value) {
    if (!index || !field || !*field || pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX) {
        return PICOWAL_SEARCH_INVALID;
    }
    int slot = find_doc_slot(index, picowal_api_key(pack, card));
    if (slot < 0) return PICOWAL_SEARCH_NOT_FOUND;
    for (uint32_t i = 0; i < index->numeric_count; i++) {
        picowal_search_numeric_entry_t *entry = &index->numerics[i];
        if (entry->doc_slot == (uint16_t)slot && strcmp(entry->field, field) == 0) {
            entry->value = value;
            return PICOWAL_SEARCH_OK;
        }
    }
    if (index->numeric_count >= PICOWAL_SEARCH_NUMERIC_ENTRY_MAX) {
        index->overflow = true;
        return PICOWAL_SEARCH_FULL;
    }
    picowal_search_numeric_entry_t *entry = &index->numerics[index->numeric_count++];
    memset(entry, 0, sizeof(*entry));
    entry->doc_slot = (uint16_t)slot;
    if (!bounded_copy(entry->field, sizeof(entry->field), field)) {
        index->numeric_count--;
        return PICOWAL_SEARCH_INVALID;
    }
    entry->value = value;
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_clear_fields(picowal_search_index_t *index,
                                                    uint16_t pack,
                                                    uint32_t card) {
    if (!index || pack < 2 || pack > PICOWAL_PACK_MAX || card > PICOWAL_CARD_MAX) {
        return PICOWAL_SEARCH_INVALID;
    }
    int slot = find_doc_slot(index, picowal_api_key(pack, card));
    if (slot < 0) return PICOWAL_SEARCH_NOT_FOUND;
    remove_doc_facets(index, (uint16_t)slot);
    remove_doc_numerics(index, (uint16_t)slot);
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_facets(const picowal_search_index_t *index,
                                              const char *field,
                                              picowal_search_facet_count_t *out,
                                              uint16_t max_out,
                                              uint16_t *out_count) {
    if (out_count) *out_count = 0;
    if (!index || !field || !*field || !out || max_out == 0 || !out_count) {
        return PICOWAL_SEARCH_INVALID;
    }
    memset(out, 0, sizeof(out[0]) * max_out);
    for (uint32_t i = 0; i < index->facet_count; i++) {
        const picowal_search_facet_entry_t *entry = &index->facets[i];
        if (strcmp(entry->field, field) != 0) continue;
        uint16_t found = max_out;
        for (uint16_t j = 0; j < *out_count; j++) {
            if (strcmp(out[j].value, entry->value) == 0) {
                found = j;
                break;
            }
        }
        if (found == max_out) {
            if (*out_count >= max_out) return PICOWAL_SEARCH_FULL;
            found = *out_count;
            bounded_copy(out[found].value, sizeof(out[found].value), entry->value);
            (*out_count)++;
        }
        out[found].count++;
    }
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_range(const picowal_search_index_t *index,
                                             const char *field,
                                             float min_value,
                                             float max_value,
                                             uint32_t *out_keys,
                                             uint16_t max_keys,
                                             uint16_t *out_count) {
    if (out_count) *out_count = 0;
    if (!index || !field || !*field || !out_keys || max_keys == 0 || !out_count || min_value > max_value) {
        return PICOWAL_SEARCH_INVALID;
    }
    for (uint32_t i = 0; i < index->numeric_count; i++) {
        const picowal_search_numeric_entry_t *entry = &index->numerics[i];
        if (strcmp(entry->field, field) != 0 || entry->value < min_value || entry->value > max_value) continue;
        if (*out_count >= max_keys) return PICOWAL_SEARCH_FULL;
        out_keys[*out_count] = index->docs[entry->doc_slot].key;
        (*out_count)++;
    }
    return PICOWAL_SEARCH_OK;
}

picowal_search_status_t picowal_search_journal_upsert(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card,
                                                      const char *text,
                                                      const float *vector,
                                                      uint16_t vector_dims) {
    if (!index) return PICOWAL_SEARCH_INVALID;
    picowal_search_status_t st = append_journal_record(journal_path, PICOWAL_SEARCH_JOURNAL_UPSERT,
                                                       pack, card, text, vector, vector_dims,
                                                       NULL, NULL, 0.0f);
    if (st != PICOWAL_SEARCH_OK) return st;
    return picowal_search_upsert(index, pack, card, text, vector, vector_dims);
}

picowal_search_status_t picowal_search_journal_delete(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card) {
    if (!index) return PICOWAL_SEARCH_INVALID;
    picowal_search_status_t st = append_journal_record(journal_path, PICOWAL_SEARCH_JOURNAL_DELETE,
                                                       pack, card, NULL, NULL, 0,
                                                       NULL, NULL, 0.0f);
    if (st != PICOWAL_SEARCH_OK) return st;
    return picowal_search_delete(index, pack, card);
}

picowal_search_status_t picowal_search_journal_facet(picowal_search_index_t *index,
                                                     const char *journal_path,
                                                     uint16_t pack,
                                                     uint32_t card,
                                                     const char *field,
                                                     const char *value) {
    if (!index) return PICOWAL_SEARCH_INVALID;
    picowal_search_status_t st = append_journal_record(journal_path, PICOWAL_SEARCH_JOURNAL_FACET,
                                                       pack, card, NULL, NULL, 0,
                                                       field, value, 0.0f);
    if (st != PICOWAL_SEARCH_OK) return st;
    return picowal_search_set_facet(index, pack, card, field, value);
}

picowal_search_status_t picowal_search_journal_number(picowal_search_index_t *index,
                                                      const char *journal_path,
                                                      uint16_t pack,
                                                      uint32_t card,
                                                      const char *field,
                                                      float value) {
    if (!index) return PICOWAL_SEARCH_INVALID;
    picowal_search_status_t st = append_journal_record(journal_path, PICOWAL_SEARCH_JOURNAL_NUMBER,
                                                       pack, card, NULL, NULL, 0,
                                                       field, NULL, value);
    if (st != PICOWAL_SEARCH_OK) return st;
    return picowal_search_set_number(index, pack, card, field, value);
}

picowal_search_status_t picowal_search_journal_replay(picowal_search_index_t *index,
                                                      const char *journal_path) {
    if (!index || !journal_path || !*journal_path) return PICOWAL_SEARCH_INVALID;
    FILE *file = fopen(journal_path, "rb");
    if (!file) return PICOWAL_SEARCH_IO;
    for (;;) {
        picowal_search_journal_header_t header;
        size_t got = fread(&header, 1, sizeof(header), file);
        if (got == 0 && feof(file)) break;
        if (got != sizeof(header)) {
            fclose(file);
            return PICOWAL_SEARCH_CORRUPT;
        }
        if (header.magic != PICOWAL_SEARCH_JOURNAL_MAGIC ||
            header.version != PICOWAL_SEARCH_JOURNAL_VERSION ||
            header.pack < 2 || header.pack > PICOWAL_PACK_MAX ||
            header.card > PICOWAL_CARD_MAX ||
            header.text_len > PICOWAL_SEARCH_TEXT_MAX ||
            header.vector_dims > PICOWAL_SEARCH_VECTOR_MAX ||
            header.field_len > PICOWAL_SEARCH_FIELD_MAX ||
            header.value_len > PICOWAL_SEARCH_FACET_VALUE_MAX ||
            header.payload_len != journal_payload_len(header.text_len, header.vector_dims,
                                                      header.field_len, header.value_len)) {
            fclose(file);
            return PICOWAL_SEARCH_CORRUPT;
        }

        uint8_t payload[PICOWAL_SEARCH_TEXT_MAX + (PICOWAL_SEARCH_VECTOR_MAX * sizeof(float)) +
                        PICOWAL_SEARCH_FIELD_MAX + PICOWAL_SEARCH_FACET_VALUE_MAX];
        if (!read_exact(file, payload, header.payload_len) ||
            search_crc32(payload, header.payload_len) != header.payload_crc32) {
            fclose(file);
            return PICOWAL_SEARCH_CORRUPT;
        }
        uint32_t off = 0;
        char text[PICOWAL_SEARCH_TEXT_MAX + 1u] = {0};
        float vector[PICOWAL_SEARCH_VECTOR_MAX] = {0};
        char field[PICOWAL_SEARCH_FIELD_MAX + 1u] = {0};
        char value[PICOWAL_SEARCH_FACET_VALUE_MAX + 1u] = {0};
        if (header.text_len) {
            memcpy(text, payload + off, header.text_len);
            off += header.text_len;
        }
        if (header.vector_dims) {
            memcpy(vector, payload + off, (size_t)header.vector_dims * sizeof(float));
            off += (uint32_t)header.vector_dims * sizeof(float);
        }
        if (header.field_len) {
            memcpy(field, payload + off, header.field_len);
            off += header.field_len;
        }
        if (header.value_len) {
            memcpy(value, payload + off, header.value_len);
            off += header.value_len;
        }

        picowal_search_status_t st = PICOWAL_SEARCH_INVALID;
        switch ((picowal_search_journal_op_t)header.op) {
            case PICOWAL_SEARCH_JOURNAL_UPSERT:
                st = picowal_search_upsert(index, header.pack, header.card, text, vector, header.vector_dims);
                break;
            case PICOWAL_SEARCH_JOURNAL_DELETE:
                st = picowal_search_delete(index, header.pack, header.card);
                if (st == PICOWAL_SEARCH_NOT_FOUND) st = PICOWAL_SEARCH_OK;
                break;
            case PICOWAL_SEARCH_JOURNAL_FACET:
                st = picowal_search_set_facet(index, header.pack, header.card, field, value);
                break;
            case PICOWAL_SEARCH_JOURNAL_NUMBER:
                st = picowal_search_set_number(index, header.pack, header.card, field, header.numeric_value);
                break;
            default:
                st = PICOWAL_SEARCH_CORRUPT;
                break;
        }
        if (st != PICOWAL_SEARCH_OK) {
            fclose(file);
            return st;
        }
    }
    return fclose(file) == 0 ? PICOWAL_SEARCH_OK : PICOWAL_SEARCH_IO;
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

picowal_search_status_t picowal_search_save(const picowal_search_index_t *index,
                                            const char *path) {
    if (!index || !path || !*path) return PICOWAL_SEARCH_INVALID;
    if (index->doc_count > PICOWAL_SEARCH_DOC_MAX ||
        index->term_count > PICOWAL_SEARCH_TERM_MAX ||
        index->posting_count > PICOWAL_SEARCH_POSTING_MAX) {
        return PICOWAL_SEARCH_CORRUPT;
    }

    picowal_search_segment_header_t header = {
        .magic = PICOWAL_SEARCH_SEG_MAGIC,
        .version = PICOWAL_SEARCH_SEG_VERSION,
        .header_size = sizeof(picowal_search_segment_header_t),
        .doc_max = PICOWAL_SEARCH_DOC_MAX,
        .term_max = PICOWAL_SEARCH_TERM_MAX,
        .posting_max = PICOWAL_SEARCH_POSTING_MAX,
        .text_max = PICOWAL_SEARCH_TEXT_MAX,
        .vector_max = PICOWAL_SEARCH_VECTOR_MAX,
        .token_max = PICOWAL_SEARCH_TOKEN_MAX,
        .doc_count = index->doc_count,
        .term_count = index->term_count,
        .flags = index->overflow ? 1u : 0u,
        .posting_count = index->posting_count,
        .facet_count = index->facet_count,
        .numeric_count = index->numeric_count,
        .vector_bucket_count = index->vector_bucket_count,
        .total_doc_len = index->total_doc_len,
        .docs_crc32 = search_crc32(index->docs, sizeof(index->docs[0]) * index->doc_count),
        .terms_crc32 = search_crc32(index->terms, sizeof(index->terms[0]) * index->term_count),
        .postings_crc32 = search_crc32(index->postings, sizeof(index->postings[0]) * index->posting_count),
        .facets_crc32 = search_crc32(index->facets, sizeof(index->facets[0]) * index->facet_count),
        .numerics_crc32 = search_crc32(index->numerics, sizeof(index->numerics[0]) * index->numeric_count),
        .vector_buckets_crc32 = search_crc32(index->vector_buckets,
                                             sizeof(index->vector_buckets[0]) * index->vector_bucket_count),
        .schema_version = index->metadata.schema_version,
        .index_generation = index->metadata.index_generation,
        .metadata_flags = index->metadata.flags,
    };
    bounded_copy(header.name, sizeof(header.name), index->metadata.name);

    FILE *file = fopen(path, "wb");
    if (!file) return PICOWAL_SEARCH_IO;
    bool ok = write_exact(file, &header, sizeof(header)) &&
              write_exact(file, index->docs, sizeof(index->docs[0]) * index->doc_count) &&
              write_exact(file, index->terms, sizeof(index->terms[0]) * index->term_count) &&
              write_exact(file, index->postings, sizeof(index->postings[0]) * index->posting_count) &&
              write_exact(file, index->facets, sizeof(index->facets[0]) * index->facet_count) &&
              write_exact(file, index->numerics, sizeof(index->numerics[0]) * index->numeric_count) &&
              write_exact(file, index->vector_buckets,
                          sizeof(index->vector_buckets[0]) * index->vector_bucket_count);
    if (fclose(file) != 0) ok = false;
    return ok ? PICOWAL_SEARCH_OK : PICOWAL_SEARCH_IO;
}

picowal_search_status_t picowal_search_load(picowal_search_index_t *index,
                                            const char *path) {
    if (!index || !path || !*path) return PICOWAL_SEARCH_INVALID;
    FILE *file = fopen(path, "rb");
    if (!file) return PICOWAL_SEARCH_IO;

    picowal_search_segment_header_t header;
    if (!read_exact(file, &header, sizeof(header))) {
        fclose(file);
        return PICOWAL_SEARCH_CORRUPT;
    }
    if (header.magic != PICOWAL_SEARCH_SEG_MAGIC ||
        header.version != PICOWAL_SEARCH_SEG_VERSION ||
        header.header_size != sizeof(picowal_search_segment_header_t) ||
        header.doc_max != PICOWAL_SEARCH_DOC_MAX ||
        header.term_max != PICOWAL_SEARCH_TERM_MAX ||
        header.posting_max != PICOWAL_SEARCH_POSTING_MAX ||
        header.text_max != PICOWAL_SEARCH_TEXT_MAX ||
        header.vector_max != PICOWAL_SEARCH_VECTOR_MAX ||
        header.token_max != PICOWAL_SEARCH_TOKEN_MAX ||
        header.doc_count > PICOWAL_SEARCH_DOC_MAX ||
        header.term_count > PICOWAL_SEARCH_TERM_MAX ||
        header.posting_count > PICOWAL_SEARCH_POSTING_MAX ||
        header.facet_count > PICOWAL_SEARCH_FACET_ENTRY_MAX ||
        header.numeric_count > PICOWAL_SEARCH_NUMERIC_ENTRY_MAX ||
        header.vector_bucket_count > PICOWAL_SEARCH_VECTOR_BUCKET_ENTRY_MAX) {
        fclose(file);
        return PICOWAL_SEARCH_CORRUPT;
    }

    picowal_search_init(index);
    index->doc_count = header.doc_count;
    index->term_count = header.term_count;
    index->posting_count = header.posting_count;
    index->facet_count = header.facet_count;
    index->numeric_count = header.numeric_count;
    index->vector_bucket_count = header.vector_bucket_count;
    index->total_doc_len = header.total_doc_len;
    index->overflow = (header.flags & 1u) != 0;
    if (picowal_search_configure(index, header.name, header.schema_version,
                                 header.index_generation, header.metadata_flags) != PICOWAL_SEARCH_OK) {
        fclose(file);
        picowal_search_init(index);
        return PICOWAL_SEARCH_CORRUPT;
    }

    bool ok = read_exact(file, index->docs, sizeof(index->docs[0]) * index->doc_count) &&
              read_exact(file, index->terms, sizeof(index->terms[0]) * index->term_count) &&
              read_exact(file, index->postings, sizeof(index->postings[0]) * index->posting_count) &&
              read_exact(file, index->facets, sizeof(index->facets[0]) * index->facet_count) &&
              read_exact(file, index->numerics, sizeof(index->numerics[0]) * index->numeric_count) &&
              read_exact(file, index->vector_buckets,
                         sizeof(index->vector_buckets[0]) * index->vector_bucket_count);
    int extra = fgetc(file);
    if (fclose(file) != 0) ok = false;
    if (!ok || extra != EOF) return PICOWAL_SEARCH_CORRUPT;

    if (search_crc32(index->docs, sizeof(index->docs[0]) * index->doc_count) != header.docs_crc32 ||
        search_crc32(index->terms, sizeof(index->terms[0]) * index->term_count) != header.terms_crc32 ||
        search_crc32(index->postings, sizeof(index->postings[0]) * index->posting_count) != header.postings_crc32 ||
        search_crc32(index->facets, sizeof(index->facets[0]) * index->facet_count) != header.facets_crc32 ||
        search_crc32(index->numerics, sizeof(index->numerics[0]) * index->numeric_count) != header.numerics_crc32 ||
        search_crc32(index->vector_buckets,
                     sizeof(index->vector_buckets[0]) * index->vector_bucket_count) != header.vector_buckets_crc32) {
        picowal_search_init(index);
        return PICOWAL_SEARCH_CORRUPT;
    }

    return PICOWAL_SEARCH_OK;
}

static void bm25_scores(const picowal_search_index_t *index, const char *query,
                        float *scores, scored_doc_t *ranked, uint16_t *ranked_count) {
    token_count_t qcounts[64];
    memset(qcounts, 0, sizeof(qcounts));
    uint16_t qn = collect_counts(query ? query : "", qcounts, 64);
    uint16_t active_count = active_doc_count(index);
    float avgdl = active_count ? (float)index->total_doc_len / (float)active_count : 0.0f;
    if (avgdl <= 0.0f) avgdl = 1.0f;
    const float k1 = 1.2f;
    const float b = 0.75f;

    for (uint16_t qi = 0; qi < qn; qi++) {
        int term_id = find_term(index, qcounts[qi].token);
        if (term_id < 0) continue;
        uint16_t df = index->terms[term_id].doc_freq;
        float idf = logf(((float)active_count - (float)df + 0.5f) / ((float)df + 0.5f) + 1.0f);
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
    uint8_t selected[PICOWAL_SEARCH_DOC_MAX] = {0};
    uint16_t hcount = 0;
    uint64_t qsig = vector_signature(query_vector, dims);
    uint16_t qbucket = vector_bucket(qsig);
    for (uint32_t i = 0; i < index->vector_bucket_count; i++) {
        const picowal_search_vector_bucket_entry_t *entry = &index->vector_buckets[i];
        if (entry->bucket != qbucket || entry->doc_slot >= index->doc_count) continue;
        const picowal_search_doc_t *doc = &index->docs[entry->doc_slot];
        if (!doc->active || doc->vector_dims != dims) continue;
        selected[entry->doc_slot] = 1;
        hamming[hcount++] = (scored_doc_t){
            .doc_slot = entry->doc_slot,
            .score = (float)popcount64(qsig ^ entry->signature),
        };
    }
    for (uint16_t i = 0; i < index->doc_count && hcount < candidate_limit; i++) {
        if (selected[i]) continue;
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
