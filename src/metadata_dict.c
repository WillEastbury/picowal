#include "metadata_dict.h"
#include "kv_flash.h"

#include <string.h>
#include <ctype.h>

typedef struct __attribute__((packed)) {
    uint8_t field_type;
    uint8_t max_len;
    char name[META_NAME_MAX + 1];
} metadata_field_record_t;

static metadata_type_def_t g_type_cache[META_MAX_ITEMS];
static metadata_field_def_t g_field_cache[META_MAX_ITEMS];
static metadata_type_schema_t g_schema_cache[META_MAX_ITEMS];
static uint32_t g_type_count = 0;
static uint32_t g_field_count = 0;
static uint32_t g_schema_count = 0;

static uint32_t metadata_key(uint16_t record_type, uint16_t ordinal) {
    return ((uint32_t)record_type << 22) | (uint32_t)ordinal;
}

static bool name_valid(const char *name) {
    size_t n = strlen(name);
    return n > 0 && n <= META_NAME_MAX;
}

static bool ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *const g_field_type_names[] = {
    "bool",
    "char",
    "char[]",
    "byte",
    "byte[]",
    "uint8",
    "int8",
    "int16",
    "int32",
    "uint16",
    "uint32",
    "isodate",
    "isotime",
    "isodatetime",
    "utf8",
    "latin1",
    "array_u16",
    "blob",
    "lookup",
};

static bool load_type_from_store(uint16_t ordinal, metadata_type_def_t *out) {
    uint16_t len = META_NAME_MAX + 1;
    char buf[META_NAME_MAX + 1];
    if (!kv_get_copy(metadata_key(META_TYPE_RECORD_TYPE, ordinal), (uint8_t *)buf, &len, NULL)) return false;
    buf[META_NAME_MAX] = '\0';
    if (out) {
        out->ordinal = ordinal;
        memcpy(out->name, buf, sizeof(out->name));
        out->name[META_NAME_MAX] = '\0';
    }
    return true;
}

static bool load_field_from_store(uint16_t ordinal, metadata_field_def_t *out) {
    metadata_field_record_t rec;
    uint16_t len = sizeof(rec);
    if (!kv_get_copy(metadata_key(META_FIELD_RECORD_TYPE, ordinal), (uint8_t *)&rec, &len, NULL)) return false;
    if (len < 2) return false;
    rec.name[META_NAME_MAX] = '\0';
    if (out) {
        out->ordinal = ordinal;
        out->field_type = rec.field_type;
        out->max_len = rec.max_len;
        memcpy(out->name, rec.name, sizeof(out->name));
        out->name[META_NAME_MAX] = '\0';
    }
    return true;
}

static bool load_schema_from_store(uint16_t type_ordinal, metadata_type_schema_t *out) {
    uint8_t buf[2 + (META_MAX_ITEMS * sizeof(uint16_t))];
    uint16_t len = sizeof(buf);
    if (!kv_get_copy(metadata_key(META_SCHEMA_RECORD_TYPE, type_ordinal), buf, &len, NULL)) return false;
    if (len < 1) return false;
    uint8_t count = buf[0];
    if (count > META_MAX_ITEMS) return false;
    if ((uint32_t)len < (uint32_t)(1 + count * sizeof(uint16_t))) return false;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->type_ordinal = type_ordinal;
        out->field_count = count;
        for (uint8_t i = 0; i < count; i++) {
            out->field_ordinals[i] = (uint16_t)(buf[1 + i * 2] | ((uint16_t)buf[2 + i * 2] << 8));
        }
    }
    return true;
}

void metadata_reload_cache(void) {
    uint32_t keys[META_MAX_ITEMS];

    g_type_count = 0;
    g_field_count = 0;
    g_schema_count = 0;

    uint32_t type_count = kv_range(metadata_key(META_TYPE_RECORD_TYPE, 0), 0xFFC00000u, keys, NULL, META_MAX_ITEMS);
    for (uint32_t i = 0; i < type_count && g_type_count < META_MAX_ITEMS; i++) {
        uint16_t ordinal = (uint16_t)(keys[i] & 0x3FFFFFu);
        if (load_type_from_store(ordinal, &g_type_cache[g_type_count])) g_type_count++;
    }

    uint32_t field_count = kv_range(metadata_key(META_FIELD_RECORD_TYPE, 0), 0xFFC00000u, keys, NULL, META_MAX_ITEMS);
    for (uint32_t i = 0; i < field_count && g_field_count < META_MAX_ITEMS; i++) {
        uint16_t ordinal = (uint16_t)(keys[i] & 0x3FFFFFu);
        if (load_field_from_store(ordinal, &g_field_cache[g_field_count])) g_field_count++;
    }

    uint32_t schema_count = kv_range(metadata_key(META_SCHEMA_RECORD_TYPE, 0), 0xFFC00000u, keys, NULL, META_MAX_ITEMS);
    for (uint32_t i = 0; i < schema_count && g_schema_count < META_MAX_ITEMS; i++) {
        uint16_t ordinal = (uint16_t)(keys[i] & 0x3FFFFFu);
        if (load_schema_from_store(ordinal, &g_schema_cache[g_schema_count])) g_schema_count++;
    }
}

// Targeted single-entry cache updaters — called after each mutation so only
// the modified entry is reloaded instead of rebuilding all three caches.
static void cache_upsert_type(uint16_t ordinal) {
    for (uint32_t i = 0; i < g_type_count; i++) {
        if (g_type_cache[i].ordinal == ordinal) {
            load_type_from_store(ordinal, &g_type_cache[i]);
            return;
        }
    }
    if (g_type_count < META_MAX_ITEMS)
        if (load_type_from_store(ordinal, &g_type_cache[g_type_count])) g_type_count++;
}

static void cache_upsert_field(uint16_t ordinal) {
    for (uint32_t i = 0; i < g_field_count; i++) {
        if (g_field_cache[i].ordinal == ordinal) {
            load_field_from_store(ordinal, &g_field_cache[i]);
            return;
        }
    }
    if (g_field_count < META_MAX_ITEMS)
        if (load_field_from_store(ordinal, &g_field_cache[g_field_count])) g_field_count++;
}

static void cache_upsert_schema(uint16_t type_ordinal) {
    for (uint32_t i = 0; i < g_schema_count; i++) {
        if (g_schema_cache[i].type_ordinal == type_ordinal) {
            load_schema_from_store(type_ordinal, &g_schema_cache[i]);
            return;
        }
    }
    if (g_schema_count < META_MAX_ITEMS)
        if (load_schema_from_store(type_ordinal, &g_schema_cache[g_schema_count])) g_schema_count++;
}

const char *metadata_field_type_name(uint8_t field_type) {
    if (field_type >= (sizeof(g_field_type_names) / sizeof(g_field_type_names[0]))) return "unknown";
    return g_field_type_names[field_type];
}

bool metadata_field_type_parse(const char *name, uint8_t *field_type) {
    for (uint8_t i = 0; i < (uint8_t)(sizeof(g_field_type_names) / sizeof(g_field_type_names[0])); i++) {
        if (ascii_ieq(name, g_field_type_names[i])) {
            *field_type = i;
            return true;
        }
    }
    if (ascii_ieq(name, "utf-8")) {
        *field_type = META_FT_UTF8;
        return true;
    }
    if (ascii_ieq(name, "latin-1")) {
        *field_type = META_FT_LATIN1;
        return true;
    }
    return false;
}

bool metadata_set_type(uint16_t ordinal, const char *name) {
    if (!name_valid(name)) return false;
    char buf[META_NAME_MAX + 1];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, name, strlen(name));
    if (!kv_put(metadata_key(META_TYPE_RECORD_TYPE, ordinal), (const uint8_t *)buf, sizeof(buf))) return false;
    cache_upsert_type(ordinal);
    return true;
}

bool metadata_get_type(uint16_t ordinal, metadata_type_def_t *out) {
    for (uint32_t i = 0; i < g_type_count; i++) {
        if (g_type_cache[i].ordinal == ordinal) {
            if (out) *out = g_type_cache[i];
            return true;
        }
    }
    return false;
}

bool metadata_find_type(const char *name, metadata_type_def_t *out) {
    for (uint32_t i = 0; i < g_type_count; i++) {
        if (ascii_ieq(name, g_type_cache[i].name)) {
            if (out) *out = g_type_cache[i];
            return true;
        }
    }
    return false;
}

uint32_t metadata_list_types(metadata_type_def_t *out, uint32_t max_items) {
    uint32_t count = (g_type_count < max_items) ? g_type_count : max_items;
    if (out) memcpy(out, g_type_cache, count * sizeof(metadata_type_def_t));
    return g_type_count;
}

bool metadata_set_field(uint16_t ordinal, const char *name, uint8_t field_type, uint8_t max_len) {
    if (!name_valid(name)) return false;
    if (strcmp(metadata_field_type_name(field_type), "unknown") == 0) return false;
    metadata_field_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.field_type = field_type;
    rec.max_len = max_len;
    memcpy(rec.name, name, strlen(name));
    if (!kv_put(metadata_key(META_FIELD_RECORD_TYPE, ordinal), (const uint8_t *)&rec, sizeof(rec))) return false;
    cache_upsert_field(ordinal);
    return true;
}

bool metadata_get_field(uint16_t ordinal, metadata_field_def_t *out) {
    for (uint32_t i = 0; i < g_field_count; i++) {
        if (g_field_cache[i].ordinal == ordinal) {
            if (out) *out = g_field_cache[i];
            return true;
        }
    }
    return false;
}

bool metadata_find_field(const char *name, metadata_field_def_t *out) {
    for (uint32_t i = 0; i < g_field_count; i++) {
        if (ascii_ieq(name, g_field_cache[i].name)) {
            if (out) *out = g_field_cache[i];
            return true;
        }
    }
    return false;
}

uint32_t metadata_list_fields(metadata_field_def_t *out, uint32_t max_items) {
    uint32_t count = (g_field_count < max_items) ? g_field_count : max_items;
    if (out) memcpy(out, g_field_cache, count * sizeof(metadata_field_def_t));
    return g_field_count;
}

bool metadata_set_schema(uint16_t type_ordinal, const uint16_t *field_ordinals, uint8_t field_count) {
    if (field_count > META_MAX_ITEMS) return false;
    uint8_t buf[1 + (META_MAX_ITEMS * sizeof(uint16_t))];
    buf[0] = field_count;
    for (uint8_t i = 0; i < field_count; i++) {
        buf[1 + i * 2] = (uint8_t)(field_ordinals[i] & 0xFFu);
        buf[2 + i * 2] = (uint8_t)(field_ordinals[i] >> 8);
    }
    if (!kv_put(metadata_key(META_SCHEMA_RECORD_TYPE, type_ordinal), buf, (uint16_t)(1 + field_count * sizeof(uint16_t)))) return false;
    cache_upsert_schema(type_ordinal);
    return true;
}

bool metadata_get_schema(uint16_t type_ordinal, metadata_type_schema_t *out) {
    for (uint32_t i = 0; i < g_schema_count; i++) {
        if (g_schema_cache[i].type_ordinal == type_ordinal) {
            if (out) *out = g_schema_cache[i];
            return true;
        }
    }
    return false;
}

uint32_t metadata_list_schemas(metadata_type_schema_t *out, uint32_t max_items) {
    uint32_t count = (g_schema_count < max_items) ? g_schema_count : max_items;
    if (out) memcpy(out, g_schema_cache, count * sizeof(metadata_type_schema_t));
    return g_schema_count;
}
