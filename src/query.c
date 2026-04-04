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
            while (*f && q.select_count < QUERY_MAX_SELECT) {
                while (*f == ' ' || *f == ',') f++;
                if (!*f) break;
                char *end = f;
                while (*end && *end != ',') end++;
                char saved = *end; *end = '\0';
                // Trim trailing spaces
                char *te = end - 1;
                while (te > f && *te == ' ') *te-- = '\0';
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
           tc == 0x04 || tc == 0x05 || tc == 0x06;
}

// ============================================================
// Execute query — scan pack, filter, project
// ============================================================

// Escape a string for pipe-delimited output: | → \|, \r → \r, \n → \n
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

static const char *g_result_pack = "";

int query_execute(const query_t *q, char *buf, int buf_size,
                  const char **pack_name, int *count) {
    *count = 0;
    *pack_name = "";

    if (!q->valid) {
        return snprintf(buf, buf_size, "error: invalid query\r\n");
    }

    // Resolve pack name → ordinal + schema
    uint32_t keys[64];
    uint32_t pack_count = kv_range(0, 0xFFC00000u, keys, NULL, 64);
    int16_t pack_ord = -1;
    uint8_t field_ords[32], field_types[32], field_count = 0;
    char field_names[32][32];

    for (uint32_t i = 0; i < pack_count; i++) {
        uint32_t pord = keys[i] & 0x3FFFFF;
        uint8_t sbuf[256]; uint16_t slen = sizeof(sbuf);
        if (!kv_get_copy(keys[i], sbuf, &slen, NULL)) continue;
        if (slen < 6 || sbuf[0] != 0x7D || sbuf[1] != 0xCA) continue;

        char pname[32] = "";
        uint16_t off = 4;
        while (off + 1 < slen) {
            uint8_t ord = sbuf[off] & 0x1F, flen = sbuf[off + 1]; off += 2;
            if (off + flen > slen) break;
            if (ord == 0 && flen >= 1) {
                uint8_t nl = sbuf[off]; if (nl > 31) nl = 31;
                memcpy(pname, sbuf + off + 1, nl); pname[nl] = '\0';
            }
            if (ord == 1 && flen >= 1) field_count = sbuf[off];
            if (ord == 2) {
                for (uint8_t fi = 0; fi < field_count && fi < 32 && fi*3+2 < flen; fi++) {
                    field_ords[fi] = sbuf[off + fi*3] & 0x1F;
                    field_types[fi] = sbuf[off + fi*3 + 1];
                }
            }
            if (ord == 5 && flen > 0) {
                uint8_t ni = 0; uint16_t si = 0;
                for (uint16_t j = 0; j < flen && ni < field_count && ni < 32; j++) {
                    if (sbuf[off+j] == '\0') {
                        uint8_t len = (uint8_t)(j - si); if (len > 31) len = 31;
                        memcpy(field_names[ni], sbuf + off + si, len);
                        field_names[ni][len] = '\0';
                        ni++; si = j + 1;
                    }
                }
            }
            off += flen;
        }

        bool match = true;
        for (int ci = 0; pname[ci] && q->from_decks[0][ci]; ci++) {
            char a = pname[ci], b = q->from_decks[0][ci];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match && strlen(pname) == strlen(q->from_decks[0])) {
            pack_ord = (int16_t)pord;
            // Store pack name for header
            static char s_pname[32];
            strncpy(s_pname, pname, 31); s_pname[31] = '\0';
            g_result_pack = s_pname;
            *pack_name = g_result_pack;
            break;
        }
    }

    if (pack_ord < 0)
        return snprintf(buf, buf_size, "error: pack '%s' not found\r\n", q->from_decks[0]);

    // Resolve WHERE fields
    uint8_t w_ords[QUERY_MAX_WHERE], w_types[QUERY_MAX_WHERE];
    for (uint8_t wi = 0; wi < q->where_count; wi++) {
        w_ords[wi] = 0xFF;
        for (uint8_t fi = 0; fi < field_count; fi++) {
            if (strcmp(q->where[wi].field, field_names[fi]) == 0) {
                w_ords[wi] = field_ords[fi]; w_types[wi] = field_types[fi]; break;
            }
        }
    }

    // Resolve SELECT fields
    uint8_t s_ords[QUERY_MAX_SELECT], s_types[QUERY_MAX_SELECT];
    uint8_t s_count = q->select_count;
    const char *s_names[QUERY_MAX_SELECT];
    if (s_count == 0) {
        s_count = field_count;
        for (uint8_t i = 0; i < field_count && i < QUERY_MAX_SELECT; i++) {
            s_ords[i] = field_ords[i]; s_types[i] = field_types[i]; s_names[i] = field_names[i];
        }
    } else {
        for (uint8_t si = 0; si < s_count; si++) {
            s_ords[si] = 0xFF; s_names[si] = q->select_fields[si].field;
            for (uint8_t fi = 0; fi < field_count; fi++) {
                if (strcmp(q->select_fields[si].field, field_names[fi]) == 0) {
                    s_ords[si] = field_ords[fi]; s_types[si] = field_types[fi]; break;
                }
            }
        }
    }

    // Scan and filter
    uint32_t card_keys[256];
    uint32_t card_count = kv_range(((uint32_t)pack_ord << 22), 0xFFC00000u, card_keys, NULL, 256);

    int n = 0;
    int result_count = 0;

    for (uint32_t ci = 0; ci < card_count && result_count < QUERY_MAX_RESULTS; ci++) {
        uint8_t card[2048]; uint16_t clen = sizeof(card);
        if (!kv_get_copy(card_keys[ci], card, &clen, NULL)) continue;

        // WHERE filter (AND)
        bool pass = true;
        for (uint8_t wi = 0; wi < q->where_count && pass; wi++) {
            if (w_ords[wi] == 0xFF) { pass = false; break; }
            char actual[64] = "";
            extract_field_str(card, clen, w_ords[wi], w_types[wi], actual, sizeof(actual));
            if (is_numeric_type(w_types[wi]))
                pass = compare_int(actual, q->where[wi].op, q->where[wi].value);
            else
                pass = compare_str(actual, q->where[wi].op, q->where[wi].value);
        }
        if (!pass) continue;

        // Output row: field1|field2|...\r\n
        for (uint8_t si = 0; si < s_count && n < buf_size - 100; si++) {
            if (si > 0 && n < buf_size - 1) buf[n++] = '|';
            if (s_ords[si] != 0xFF) {
                char val[64] = "";
                extract_field_str(card, clen, s_ords[si], s_types[si], val, sizeof(val));
                n += escape_field(buf + n, buf_size - n, val);
            }
        }
        if (n < buf_size - 2) { buf[n++] = '\r'; buf[n++] = '\n'; }
        result_count++;
    }

    buf[n] = '\0';
    *count = result_count;
    return n;
}
