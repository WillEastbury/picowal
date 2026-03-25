#ifndef METADATA_DICT_H
#define METADATA_DICT_H

#include <stdbool.h>
#include <stdint.h>

#define META_NAME_MAX 31u
#define META_MAX_ITEMS 64u
#define META_TYPE_RECORD_TYPE 1022u
#define META_FIELD_RECORD_TYPE 1023u
#define META_SCHEMA_RECORD_TYPE 0u

typedef enum {
    META_FT_BOOL = 0,
    META_FT_CHAR,
    META_FT_CHAR_ARRAY,
    META_FT_BYTE,
    META_FT_BYTE_ARRAY,
    META_FT_UINT8,
    META_FT_INT8,
    META_FT_INT16,
    META_FT_INT32,
    META_FT_UINT16,
    META_FT_UINT32,
    META_FT_ISODATE,
    META_FT_ISOTIME,
    META_FT_ISODATETIME,
    META_FT_UTF8,
    META_FT_LATIN1,
} metadata_field_type_t;

typedef struct {
    uint16_t ordinal;
    char name[META_NAME_MAX + 1];
} metadata_type_def_t;

typedef struct {
    uint16_t ordinal;
    char name[META_NAME_MAX + 1];
    uint8_t field_type;
    uint8_t max_len;
} metadata_field_def_t;

typedef struct {
    uint16_t type_ordinal;
    uint8_t field_count;
    uint16_t field_ordinals[META_MAX_ITEMS];
} metadata_type_schema_t;

void metadata_reload_cache(void);

const char *metadata_field_type_name(uint8_t field_type);
bool metadata_field_type_parse(const char *name, uint8_t *field_type);

bool metadata_set_type(uint16_t ordinal, const char *name);
bool metadata_get_type(uint16_t ordinal, metadata_type_def_t *out);
bool metadata_find_type(const char *name, metadata_type_def_t *out);
uint32_t metadata_list_types(metadata_type_def_t *out, uint32_t max_items);

bool metadata_set_field(uint16_t ordinal, const char *name, uint8_t field_type, uint8_t max_len);
bool metadata_get_field(uint16_t ordinal, metadata_field_def_t *out);
bool metadata_find_field(const char *name, metadata_field_def_t *out);
uint32_t metadata_list_fields(metadata_field_def_t *out, uint32_t max_items);

bool metadata_set_schema(uint16_t type_ordinal, const uint16_t *field_ordinals, uint8_t field_count);
bool metadata_get_schema(uint16_t type_ordinal, metadata_type_schema_t *out);
uint32_t metadata_list_schemas(metadata_type_schema_t *out, uint32_t max_items);

#endif
