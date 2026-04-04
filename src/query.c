#include "query.h"
#include "kv_flash.h"
#include "kv_sd.h"
#include "metadata_dict.h"
#include "httpd/web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// ============================================================
// Query Parser
// ============================================================

static query_op_t parse_op(const char *s) {
    if (strcmp(s, "==") == 0) return QOP_EQ;
    if (strcmp(s, "!=") == 0) return QOP_NE;
    if (strcmp(s, ">")  == 0) return QOP_GT;
    if (strcmp(s, "<")  == 0) return QOP_LT;
    if (strcmp(s, ">=") == 0) return QOP_GE;
    if (strcmp(s, "<=") == 0) return QOP_LE;
    if (strcmp(s, "IN") == 0 || strcmp(s, "in") == 0) return QOP_IN;
    if (strcmp(s, "NI") == 0 || strcmp(s, "ni") == 0) return QOP_NI;
    return QOP_EQ;
}


static int strcasecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}
// Split "pack.field" into pack and field parts. If no dot, pack is empty.
static void split_dotted(const char *s, char *pack, int pack_max, char *field, int field_max) {
    const char *dot = strchr(s, '.');
    if (dot) {
        int plen = (int)(dot - s); if (plen >= pack_max) plen = pack_max - 1;
        memcpy(pack, s, plen); pack[plen] = '\0';
        strncpy(field, dot + 1, field_max - 1); field[field_max - 1] = '\0';
    } else {
        pack[0] = '\0';
        strncpy(field, s, field_max - 1); field[field_max - 1] = '\0';
    }
}
query_t query_parse(const char *text) {
    query_t q;
    memset(&q, 0, sizeof(q));
    q.valid = false;

    // Pre-split into lines (max 16)
    char buf[512];
    uint16_t tlen = (uint16_t)strlen(text);
    if (tlen >= sizeof(buf)) return q;
    memcpy(buf, text, tlen + 1);
    // Sanitize: replace \r with \n, strip all other control chars
    for (uint16_t i = 0; i < tlen; i++) {
        char c = buf[i];
        if (c == '\r') buf[i] = '\n';
        else if (c < 0x20 && c != '\n') buf[i] = ' ';
    }

    char *lines[16];
    int nlines = 0;
    char *p = buf;
    while (*p && nlines < 16) {
        while (*p == '\r' || *p == '\n') *p++ = '\0';
        if (!*p) break;
        lines[nlines++] = p;
        while (*p && *p != '\r' && *p != '\n') p++;
    }

    // Process each line
    for (int li = 0; li < nlines; li++) {
        char *line = lines[li];
        while (*line == ' ') line++;

        if (line[0] == 'S' && line[1] == ':') {
            char *f = line + 2;
            while (*f == ' ') f++;
            // S:* means select all fields (leave select_count = 0)
            if (*f == '*') { q.select_count = 0; }
            else while (*f && q.select_count < QUERY_MAX_SELECT) {
                while (*f == ' ' || *f == ',') f++;
                if (!*f) break;
                char *end = f;
                while (*end && *end != ',') end++;
                char saved = *end; *end = '\0';
                // Trim trailing spaces
                char *te = end - 1;
                while (te > f && *te == ' ') *te-- = '\0';
                // Check for AGG|field syntax (e.g. SUM|price)
                q.select_fields[q.select_count].agg = QAGG_NONE;
                char *pipe = strchr(f, '|');
                if (pipe) {
                    *pipe = '\0';
                    if (strcmp(f,"SUM")==0||strcmp(f,"sum")==0) q.select_fields[q.select_count].agg = QAGG_SUM;
                    else if (strcmp(f,"AVG")==0||strcmp(f,"avg")==0) q.select_fields[q.select_count].agg = QAGG_AVG;
                    else if (strcmp(f,"MIN")==0||strcmp(f,"min")==0) q.select_fields[q.select_count].agg = QAGG_MIN;
                    else if (strcmp(f,"MAX")==0||strcmp(f,"max")==0) q.select_fields[q.select_count].agg = QAGG_MAX;
                    else if (strcmp(f,"COUNT")==0||strcmp(f,"count")==0) q.select_fields[q.select_count].agg = QAGG_COUNT;
                    else if (strcmp(f,"FIRST")==0||strcmp(f,"first")==0) q.select_fields[q.select_count].agg = QAGG_FIRST;
                    f = pipe + 1;
                    while (*f == ' ') f++;
                }
                split_dotted(f, q.select_fields[q.select_count].pack, 32,
                             q.select_fields[q.select_count].field, 32);
                q.select_count++;
                *end = saved;
                f = (*end) ? end + 1 : end;
            }
        } else if (line[0] == 'F' && line[1] == ':') {
            char *f = line + 2;
            while (*f && q.from_count < 4) {
                while (*f == ' ' || *f == ',') f++;
                if (!*f) break;
                char *end = f;
                while (*end && *end != ',') end++;
                char saved = *end; *end = '\0';
                char *te = end - 1;
                while (te > f && *te == ' ') *te-- = '\0';
                strncpy(q.from_decks[q.from_count], f, 31);
                q.from_decks[q.from_count][31] = '\0';
                q.from_count++;
                *end = saved;
                f = (*end) ? end + 1 : end;
            }
        } else if (line[0] == 'W' && line[1] == ':' && q.where_count < QUERY_MAX_WHERE) {
            char *rest = line + 2;
            char *p1 = strchr(rest, '|');
            if (p1) {
                *p1 = '\0';
                char *p2 = strchr(p1 + 1, '|');
                if (p2) {
                    *p2 = '\0';
                    char *field = rest; while (*field == ' ') field++;
                    split_dotted(field, q.where[q.where_count].pack, 32,
                                 q.where[q.where_count].field, 32);
                    char *op_str = p1 + 1; while (*op_str == ' ') op_str++;
                    q.where[q.where_count].op = parse_op(op_str);
                    char *val = p2 + 1; while (*val == ' ') val++;
                    strncpy(q.where[q.where_count].value, val, 63);
                    q.where_count++;
                }
            }
        }
    }

    q.valid = (q.from_count > 0);
    return q;
}

// ============================================================
// Field value extraction from card binary
// ============================================================

// Decode a field value from a 0xCA7D card as a string
static bool extract_field_str(const uint8_t *card, uint16_t card_len,
                              uint8_t target_ord, uint8_t type_code,
                              char *out, int out_max) {
    if (card_len < 4 || card[0] != 0x7D || card[1] != 0xCA) return false;
    uint16_t off = 4;
    while (off + 1 < card_len) {
        uint8_t ord = card[off] & 0x1F;
        uint8_t flen = card[off + 1];
        off += 2;
        if (off + flen > card_len) break;

        if (ord == target_ord) {
            // Decode based on type
            switch (type_code) {
                case 0x07: // bool
                    snprintf(out, out_max, "%s", (flen && card[off]) ? "true" : "false");
                    return true;
                case 0x01: // uint8
                    snprintf(out, out_max, "%u", flen ? card[off] : 0);
                    return true;
                case 0x02: { // uint16
                    uint16_t v = 0; if (flen >= 2) memcpy(&v, card + off, 2);
                    snprintf(out, out_max, "%u", v); return true;
                }
                case 0x03: { // uint32
                    uint32_t v = 0; if (flen >= 4) memcpy(&v, card + off, 4);
                    snprintf(out, out_max, "%lu", (unsigned long)v); return true;
                }
                case 0x04: // int8
                    snprintf(out, out_max, "%d", flen ? (int8_t)card[off] : 0);
                    return true;
                case 0x05: { // int16
                    int16_t v = 0; if (flen >= 2) memcpy(&v, card + off, 2);
                    snprintf(out, out_max, "%d", (int)v); return true;
                }
                case 0x06: { // int32
                    int32_t v = 0; if (flen >= 4) memcpy(&v, card + off, 4);
                    snprintf(out, out_max, "%ld", (long)v); return true;
                }
                case 0x08: case 0x09: // ascii, utf8 (length-prefixed)
                    if (flen >= 1) {
                        uint8_t slen = card[off];
                        if (slen > flen - 1) slen = flen - 1;
                        if (slen >= (uint8_t)out_max) slen = (uint8_t)(out_max - 1);
                        memcpy(out, card + off + 1, slen);
                        out[slen] = '\0';
                    } else { out[0] = '\0'; }
                    return true;
                case 0x10: { // array_u16
                    int n = 0;
                    if (flen >= 1) {
                        uint8_t bc = card[off];
                        for (uint8_t i = 0; i + 1 < bc && n < out_max - 6; i += 2) {
                            uint16_t v = card[off+1+i] | ((uint16_t)card[off+2+i] << 8);
                            if (n > 0) out[n++] = ',';
                            n += snprintf(out + n, out_max - n, "%u", v);
                        }
                    }
                    out[n] = '\0'; return true;
                }
                case 0x12: { // lookup — stored as uint32
                    uint32_t v = 0; if (flen >= 4) memcpy(&v, card + off, 4);
                    snprintf(out, out_max, "%lu", (unsigned long)v); return true;
                }
                default:
                    out[0] = '\0'; return true;
            }
        }
        off += flen;
    }
    out[0] = '\0';
    return false;
}

// ============================================================
// Comparison operators
// ============================================================

static bool compare_str(const char *actual, query_op_t op, const char *expected) {
    int cmp = strcmp(actual, expected);
    switch (op) {
        case QOP_EQ: return cmp == 0;
        case QOP_NE: return cmp != 0;
        case QOP_GT: return cmp > 0;
        case QOP_LT: return cmp < 0;
        case QOP_GE: return cmp >= 0;
        case QOP_LE: return cmp <= 0;
        case QOP_IN: {
            // Check if actual is in comma-separated expected
            char buf[64]; strncpy(buf, expected, 63); buf[63] = '\0';
            char *tok = strtok(buf, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (strcmp(actual, tok) == 0) return true;
                tok = strtok(NULL, ",");
            }
            return false;
        }
        case QOP_NI: {
            char buf[64]; strncpy(buf, expected, 63); buf[63] = '\0';
            char *tok = strtok(buf, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (strcmp(actual, tok) == 0) return false;
                tok = strtok(NULL, ",");
            }
            return true;
        }
    }
    return false;
}

static bool compare_int(const char *actual, query_op_t op, const char *expected) {
    long a = strtol(actual, NULL, 10);
    long e = strtol(expected, NULL, 10);
    switch (op) {
        case QOP_EQ: return a == e;
        case QOP_NE: return a != e;
        case QOP_GT: return a > e;
        case QOP_LT: return a < e;
        case QOP_GE: return a >= e;
        case QOP_LE: return a <= e;
        case QOP_IN: case QOP_NI: return compare_str(actual, op, expected);
    }
    return false;
}

static bool is_numeric_type(uint8_t tc) {
    return tc == 0x01 || tc == 0x02 || tc == 0x03 ||
           tc == 0x04 || tc == 0x05 || tc == 0x06 ||
           tc == 0x12; // lookup stored as uint32
}

// ============================================================
// Execute query — scan pack, filter, project
// ============================================================

// Escape for pipe-delimited output
static int escape_field(char *out, int max, const char *val) {
    int n = 0;
    for (const char *p = val; *p && n < max - 2; p++) {
        if (*p == '|') { out[n++] = '\\'; out[n++] = '|'; }
        else if (*p == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (*p == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (*p == '\\') { out[n++] = '\\'; out[n++] = '\\'; }
        else out[n++] = *p;
    }
    out[n] = '\0';
    return n;
}

// Resolve a pack name to ordinal + schema. Returns -1 if not found.
typedef struct {
    int16_t  ord;
    uint8_t  field_ords[32];
    uint8_t  field_types[32];
    uint8_t  field_maxlens[32];
    char     field_names[32][32];
    uint8_t  field_count;
    char     name[32];
} pack_schema_t;

static bool resolve_pack(const char *name, pack_schema_t *ps) {
    uint32_t keys[64];
    uint32_t count = kv_range(0, 0xFFC00000u, keys, NULL, 64);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
        if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
        if (slen < 6 || sbuf[0] != 0x7D || sbuf[1] != 0xCA) continue;

        char pname[32] = "";
        uint8_t fc = 0;
        uint16_t off = 4;
        while (off + 1 < slen) {
            uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off+1]; off += 2;
            if (off + flen > slen) break;
            if (ord == 0 && flen >= 1) {
                uint8_t nl = sbuf[off]; if (nl > 31) nl = 31;
                memcpy(pname, sbuf+off+1, nl); pname[nl] = '\0';
            }
            if (ord == 1 && flen >= 1) fc = sbuf[off];
            if (ord == 2) {
                for (uint8_t fi = 0; fi < fc && fi < 32 && fi*3+2 < flen; fi++) {
                    ps->field_ords[fi] = sbuf[off+fi*3] & 0x1F;
                    ps->field_types[fi] = sbuf[off+fi*3+1];
                    ps->field_maxlens[fi] = sbuf[off+fi*3+2];
                }
            }
            if (ord == 5 && flen > 0) {
                uint8_t ni = 0; uint16_t si = 0;
                for (uint16_t j = 0; j < flen && ni < fc && ni < 32; j++) {
                    if (sbuf[off+j] == '\0') {
                        uint8_t len = (uint8_t)(j-si); if (len > 31) len = 31;
                        memcpy(ps->field_names[ni], sbuf+off+si, len);
                        ps->field_names[ni][len] = '\0';
                        ni++; si = j + 1;
                    }
                }
            }
            off += flen;
        }

        // Case-insensitive compare
        bool match = (strlen(pname) == strlen(name));
        for (int ci = 0; match && pname[ci]; ci++) {
            char a = pname[ci], b = name[ci];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) match = false;
        }
        if (match) {
            ps->ord = (int16_t)(keys[i] & 0x3FFFFF);
            ps->field_count = fc;
            strncpy(ps->name, pname, 31);
            return true;
        }
    }
    return false;
}

// Find a field in a pack schema by name. Returns field index or -1.
static int8_t find_field(const pack_schema_t *ps, const char *name) {
    for (uint8_t i = 0; i < ps->field_count; i++) {
        if (strcmp(ps->field_names[i], name) == 0) return (int8_t)i;
    }
    return -1;
}

// Find which field in pack A is a lookup to pack B. Returns field index or -1.
static int8_t find_lookup_to(const pack_schema_t *from, const pack_schema_t *to) {
    for (uint8_t i = 0; i < from->field_count; i++) {
        if (from->field_types[i] == 0x12) { // lookup type
            if (from->field_maxlens[i] == (uint8_t)to->ord) return (int8_t)i;
        }
    }
    return -1;
}

// Search a pack for cards matching field == value, return card IDs
static uint32_t search_pack_field(const pack_schema_t *ps, uint8_t field_idx,
                                   const char *value, query_op_t op,
                                   uint32_t *out_ids, uint32_t max) {
    uint32_t keys[256];
    uint32_t count = kv_range(((uint32_t)ps->ord << 22), 0xFFC00000u, keys, NULL, 256);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count && found < max; i++) {
        uint8_t card[2048]; uint16_t clen = sizeof(card);
        if (!kv_get_copy(keys[i], card, &clen, NULL)) continue;
        char actual[64] = "";
        extract_field_str(card, clen, ps->field_ords[field_idx],
                         ps->field_types[field_idx], actual, sizeof(actual));
        bool pass;
        if (is_numeric_type(ps->field_types[field_idx]))
            pass = compare_int(actual, op, value);
        else
            pass = compare_str(actual, op, value);
        if (pass) out_ids[found++] = keys[i] & 0x3FFFFF;
    }
    return found;
}

static const char *g_result_pack = "";

int query_execute(const query_t *q, char *buf, int buf_size,
                  const char **pack_name, int *count) {
    *count = 0;
    *pack_name = "";

    if (!q->valid || q->from_count == 0)
        return snprintf(buf, buf_size, "error: invalid query\r\n");

    // Resolve all FROM packs
    pack_schema_t packs[4];
    uint8_t pack_resolved = 0;
    for (uint8_t i = 0; i < q->from_count && i < 4; i++) {
        if (resolve_pack(q->from_decks[i], &packs[i])) pack_resolved++;
        else return snprintf(buf, buf_size, "error: pack '%s' not found\r\n", q->from_decks[i]);
    }

    // Primary pack = first FROM
    pack_schema_t *primary = &packs[0];
    static char s_pname[32];
    strncpy(s_pname, primary->name, 31);
    g_result_pack = s_pname;
    *pack_name = g_result_pack;

    // Process WHERE clauses — resolve cross-pack references via lookups
    // Rewritten WHERE: all targeting primary pack
    uint8_t w_ords[QUERY_MAX_WHERE], w_types[QUERY_MAX_WHERE];
    char    w_values[QUERY_MAX_WHERE][64];
    query_op_t w_ops[QUERY_MAX_WHERE];
    uint8_t w_count = 0;

    for (uint8_t wi = 0; wi < q->where_count; wi++) {
        const char *wpname = q->where[wi].pack;
        const char *wfield = q->where[wi].field;

        if (wpname[0] == '\0' || strcmp(wpname, primary->name) == 0 ||
            strcasecmp_local(wpname, q->from_decks[0]) == 0) {
            // WHERE on primary pack — direct
            int8_t fi = find_field(primary, wfield);
            if (fi < 0) return snprintf(buf, buf_size, "error: field '%s' not found in '%s'\r\n", wfield, primary->name);
            w_ords[w_count] = primary->field_ords[fi];
            w_types[w_count] = primary->field_types[fi];
            w_ops[w_count] = q->where[wi].op;
            strncpy(w_values[w_count], q->where[wi].value, 63);
            w_count++;
        } else {
            // WHERE on joined pack — resolve via lookup
            // Find which joined pack this refers to
            pack_schema_t *joined = NULL;
            for (uint8_t ji = 1; ji < q->from_count; ji++) {
                if (strcasecmp_local(wpname, q->from_decks[ji]) == 0 ||
                    strcmp(wpname, packs[ji].name) == 0) {
                    joined = &packs[ji]; break;
                }
            }
            if (!joined) return snprintf(buf, buf_size, "error: pack '%s' not in FROM\r\n", wpname);

            // Find the field on the joined pack
            int8_t jfi = find_field(joined, wfield);
            if (jfi < 0) return snprintf(buf, buf_size, "error: field '%s' not found in '%s'\r\n", wfield, joined->name);

            // Search joined pack for matching card IDs
            uint32_t matched_ids[64];
            uint32_t nmatched = search_pack_field(joined, (uint8_t)jfi,
                                                   q->where[wi].value, q->where[wi].op,
                                                   matched_ids, 64);
            if (nmatched == 0) { *count = 0; buf[0] = '\0'; return 0; }

            // Find lookup field in primary that points to joined pack
            int8_t lf = find_lookup_to(primary, joined);
            if (lf < 0) return snprintf(buf, buf_size, "error: no lookup from '%s' to '%s'\r\n", primary->name, joined->name);

            // Rewrite WHERE: lookup_field IN matched_ids
            w_ords[w_count] = primary->field_ords[lf];
            w_types[w_count] = primary->field_types[lf];
            w_ops[w_count] = QOP_IN;
            // Build comma-separated ID list
            char idlist[64] = "";
            int idlen = 0;
            for (uint32_t mi = 0; mi < nmatched && idlen < 60; mi++) {
                if (mi > 0) idlist[idlen++] = ',';
                idlen += snprintf(idlist + idlen, sizeof(idlist) - idlen, "%lu", (unsigned long)matched_ids[mi]);
            }
            strncpy(w_values[w_count], idlist, 63);
            w_count++;
        }
    }

    // Resolve SELECT fields (may reference joined packs)
    uint8_t s_ords[QUERY_MAX_SELECT], s_types[QUERY_MAX_SELECT];
    const char *s_names[QUERY_MAX_SELECT];
    int8_t s_pack_idx[QUERY_MAX_SELECT]; // -1 = primary, 0+ = joined pack index
    uint8_t s_count = q->select_count;

    if (s_count == 0) {
        s_count = primary->field_count;
        for (uint8_t i = 0; i < s_count && i < QUERY_MAX_SELECT; i++) {
            s_ords[i] = primary->field_ords[i];
            s_types[i] = primary->field_types[i];
            s_names[i] = primary->field_names[i];
            s_pack_idx[i] = -1;
        }
    } else {
        for (uint8_t si = 0; si < s_count; si++) {
            const char *spname = q->select_fields[si].pack;
            const char *sfield = q->select_fields[si].field;
            s_ords[si] = 0xFF;
            s_pack_idx[si] = -1;
            s_names[si] = sfield;

            // Determine which pack
            pack_schema_t *target = primary;
            if (spname[0] != '\0') {
                for (uint8_t pi = 0; pi < q->from_count; pi++) {
                    if (strcasecmp_local(spname, q->from_decks[pi]) == 0 ||
                        strcmp(spname, packs[pi].name) == 0) {
                        target = &packs[pi];
                        s_pack_idx[si] = (int8_t)pi;
                        break;
                    }
                }
            }
            int8_t fi = find_field(target, sfield);
            if (fi >= 0) { s_ords[si] = target->field_ords[fi]; s_types[si] = target->field_types[fi]; }
        }
    }

    // Check if any aggregates are requested
    bool has_agg = false;
    for (uint8_t si = 0; si < s_count; si++) {
        if (q->select_fields[si].agg != QAGG_NONE) { has_agg = true; break; }
    }
    web_log("[query] s_count=%d has_agg=%d\n", s_count, has_agg);
    for (uint8_t si = 0; si < s_count; si++) {
        web_log("[query]  S[%d]: agg=%d field='%s' ord=%d type=0x%02x\n",
                si, q->select_fields[si].agg, s_names[si], s_ords[si], s_types[si]);
    }

    // Scan primary pack, filter
    uint32_t card_keys[256];
    uint32_t card_count = kv_range(((uint32_t)primary->ord << 22), 0xFFC00000u, card_keys, NULL, 256);

    if (!has_agg) {
        // No aggregates — direct output per row
        int n = 0;
        int result_count = 0;
        for (uint32_t ci = 0; ci < card_count && result_count < QUERY_MAX_RESULTS; ci++) {
            uint8_t card[2048]; uint16_t clen = sizeof(card);
            if (!kv_get_copy(card_keys[ci], card, &clen, NULL)) continue;

            bool pass = true;
            for (uint8_t wi = 0; wi < w_count && pass; wi++) {
                char actual[64] = "";
                extract_field_str(card, clen, w_ords[wi], w_types[wi], actual, sizeof(actual));
                if (is_numeric_type(w_types[wi]))
                    pass = compare_int(actual, w_ops[wi], w_values[wi]);
                else
                    pass = compare_str(actual, w_ops[wi], w_values[wi]);
            }
            if (!pass) continue;

            for (uint8_t si = 0; si < s_count && n < buf_size - 100; si++) {
                if (si > 0 && n < buf_size - 1) buf[n++] = '|';
                if (s_ords[si] == 0xFF) continue;
                if (s_pack_idx[si] <= 0) {
                    char val[64] = "";
                    extract_field_str(card, clen, s_ords[si], s_types[si], val, sizeof(val));
                    n += escape_field(buf + n, buf_size - n, val);
                } else {
                    int8_t lf = find_lookup_to(primary, &packs[s_pack_idx[si]]);
                    if (lf >= 0) {
                        char ref_id_str[16] = "";
                        extract_field_str(card, clen, primary->field_ords[lf], primary->field_types[lf], ref_id_str, sizeof(ref_id_str));
                        uint32_t ref_id = (uint32_t)strtoul(ref_id_str, NULL, 10);
                        uint32_t ref_key = ((uint32_t)packs[s_pack_idx[si]].ord << 22) | ref_id;
                        uint8_t ref_card[2048]; uint16_t rclen = sizeof(ref_card);
                        if (kv_get_copy(ref_key, ref_card, &rclen, NULL)) {
                            char val[64] = "";
                            extract_field_str(ref_card, rclen, s_ords[si], s_types[si], val, sizeof(val));
                            n += escape_field(buf + n, buf_size - n, val);
                        }
                    }
                }
            }
            if (n < buf_size - 2) { buf[n++] = '\r'; buf[n++] = '\n'; }
            result_count++;
        }
        buf[n] = '\0';
        *count = result_count;
        return n;
    }

    // ---- Aggregation path ----
    // Collect values per field for all matching rows
    #define AGG_MAX_ROWS 256
    #define AGG_MAX_GROUPS 64
    char row_vals[AGG_MAX_ROWS][QUERY_MAX_SELECT][32];
    int row_count = 0;

    for (uint32_t ci = 0; ci < card_count && row_count < AGG_MAX_ROWS; ci++) {
        uint8_t card[2048]; uint16_t clen = sizeof(card);
        if (!kv_get_copy(card_keys[ci], card, &clen, NULL)) continue;

        bool pass = true;
        for (uint8_t wi = 0; wi < w_count && pass; wi++) {
            char actual[64] = "";
            extract_field_str(card, clen, w_ords[wi], w_types[wi], actual, sizeof(actual));
            if (is_numeric_type(w_types[wi]))
                pass = compare_int(actual, w_ops[wi], w_values[wi]);
            else
                pass = compare_str(actual, w_ops[wi], w_values[wi]);
        }
        if (!pass) continue;

        for (uint8_t si = 0; si < s_count; si++) {
            row_vals[row_count][si][0] = '\0';
            if (s_ords[si] == 0xFF) continue;
            if (s_pack_idx[si] <= 0) {
                extract_field_str(card, clen, s_ords[si], s_types[si], row_vals[row_count][si], 32);
            } else {
                int8_t lf = find_lookup_to(primary, &packs[s_pack_idx[si]]);
                if (lf >= 0) {
                    char ref_id_str[16] = "";
                    extract_field_str(card, clen, primary->field_ords[lf], primary->field_types[lf], ref_id_str, sizeof(ref_id_str));
                    uint32_t ref_id = (uint32_t)strtoul(ref_id_str, NULL, 10);
                    uint32_t ref_key = ((uint32_t)packs[s_pack_idx[si]].ord << 22) | ref_id;
                    uint8_t ref_card[2048]; uint16_t rclen = sizeof(ref_card);
                    if (kv_get_copy(ref_key, ref_card, &rclen, NULL))
                        extract_field_str(ref_card, rclen, s_ords[si], s_types[si], row_vals[row_count][si], 32);
                }
            }
        }
        row_count++;
    }

    // Group by non-aggregate fields, compute aggregates
    // Group key = concatenation of all QAGG_NONE field values
    char group_keys[AGG_MAX_GROUPS][128];
    long  agg_vals[AGG_MAX_GROUPS][QUERY_MAX_SELECT]; // running aggregates
    int   agg_counts[AGG_MAX_GROUPS];
    char  agg_first[AGG_MAX_GROUPS][QUERY_MAX_SELECT][32]; // FIRST values
    int   group_count = 0;

    for (int ri = 0; ri < row_count; ri++) {
        // Build group key
        char gk[128] = "";
        int gklen = 0;
        for (uint8_t si = 0; si < s_count; si++) {
            if (q->select_fields[si].agg == QAGG_NONE) {
                if (gklen > 0) gk[gklen++] = '|';
                int sl = (int)strlen(row_vals[ri][si]);
                memcpy(gk + gklen, row_vals[ri][si], sl);
                gklen += sl;
            }
        }
        gk[gklen] = '\0';

        // Find or create group
        int gi = -1;
        for (int g = 0; g < group_count; g++) {
            if (strcmp(group_keys[g], gk) == 0) { gi = g; break; }
        }
        if (gi < 0 && group_count < AGG_MAX_GROUPS) {
            gi = group_count++;
            strcpy(group_keys[gi], gk);
            agg_counts[gi] = 0;
            for (uint8_t si = 0; si < s_count; si++) {
                agg_vals[gi][si] = 0;
                if (q->select_fields[si].agg == QAGG_MIN) agg_vals[gi][si] = 2147483647L;
                if (q->select_fields[si].agg == QAGG_MAX) agg_vals[gi][si] = -2147483647L;
                strncpy(agg_first[gi][si], row_vals[ri][si], 31);
            }
        }
        if (gi < 0) continue;

        agg_counts[gi]++;
        for (uint8_t si = 0; si < s_count; si++) {
            long v = strtol(row_vals[ri][si], NULL, 10);
            switch (q->select_fields[si].agg) {
                case QAGG_SUM: agg_vals[gi][si] += v; break;
                case QAGG_AVG: agg_vals[gi][si] += v; break;
                case QAGG_MIN: if (v < agg_vals[gi][si]) agg_vals[gi][si] = v; break;
                case QAGG_MAX: if (v > agg_vals[gi][si]) agg_vals[gi][si] = v; break;
                default: break;
            }
        }
    }

    // Output grouped results
    int n = 0;
    for (int gi = 0; gi < group_count && n < buf_size - 100; gi++) {
        for (uint8_t si = 0; si < s_count; si++) {
            if (si > 0 && n < buf_size - 1) buf[n++] = '|';
            switch (q->select_fields[si].agg) {
                case QAGG_SUM:
                    n += snprintf(buf + n, buf_size - n, "%ld", agg_vals[gi][si]);
                    break;
                case QAGG_AVG:
                    n += snprintf(buf + n, buf_size - n, "%ld",
                                 agg_counts[gi] ? agg_vals[gi][si] / agg_counts[gi] : 0);
                    break;
                case QAGG_MIN: case QAGG_MAX:
                    n += snprintf(buf + n, buf_size - n, "%ld", agg_vals[gi][si]);
                    break;
                case QAGG_COUNT:
                    n += snprintf(buf + n, buf_size - n, "%d", agg_counts[gi]);
                    break;
                case QAGG_FIRST:
                    n += escape_field(buf + n, buf_size - n, agg_first[gi][si]);
                    break;
                case QAGG_NONE:
                    n += escape_field(buf + n, buf_size - n, agg_first[gi][si]);
                    break;
            }
        }
        if (n < buf_size - 2) { buf[n++] = '\r'; buf[n++] = '\n'; }
    }

    buf[n] = '\0';
    *count = group_count;
    return n;
}
