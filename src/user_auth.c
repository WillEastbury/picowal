#include "user_auth.h"
#include "kv_flash.h"
#include "pico/stdlib.h"
#include "pico/rand.h"

#include <string.h>
#include <stdio.h>

// Pico SDK hardware SHA-256 (RP2350 accelerator)
#include "pico/sha256.h"

// ============================================================
// Binary card format (matches picowal.js CARD_MAGIC 0xCA7D)
//
// [magic:2][version:2][field0][field1]...
// Each field: [ord_byte:1][len:1][data:len]
//   ord_byte = (flags:3 << 5) | (ordinal:5)
// ============================================================

#define CARD_MAGIC_LO  0x7D
#define CARD_MAGIC_HI  0xCA

// Key packing: Pack P, card N → key = (P << 22) | N
#define PACK_KEY(pack, card) (((uint32_t)(pack) << 22) | ((card) & 0x3FFFFF))
#define USER_KEY(card)       PACK_KEY(USER_PACK, card)
#define SCHEMA_KEY(pack)     PACK_KEY(0, pack)      // Pack 0 = metadata schemas

// picowal.js field type codes (T.*)
#define FT_UINT8      0x01
#define FT_UINT16     0x02
#define FT_UINT32     0x03
#define FT_INT8       0x04
#define FT_INT16      0x05
#define FT_INT32      0x06
#define FT_BOOL       0x07
#define FT_ASCII      0x08
#define FT_UTF8       0x09
#define FT_ARRAY_U16  0x10
#define FT_BLOB       0x11
#define FT_LOOKUP     0x12

// ============================================================
// Session table (in-memory)
// ============================================================

typedef struct {
    uint8_t  token[SESSION_TOKEN_LEN];
    uint32_t user_card;
    uint32_t created_ms;
    uint32_t last_ms;
    bool     active;
} session_slot_t;

static session_slot_t g_sessions[SESSION_MAX];

// ============================================================
// Helpers
// ============================================================

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void sha256_hash(const uint8_t *salt, uint8_t slen,
                        const char *pass, uint8_t plen,
                        uint8_t hash_out[USER_HASH_LEN]) {
    pico_sha256_state_t state;
    if (pico_sha256_try_start(&state, SHA256_BIG_ENDIAN, true) != PICO_OK) return;
    pico_sha256_update(&state, salt, slen);
    pico_sha256_update(&state, (const uint8_t *)pass, plen);
    sha256_result_t result;
    pico_sha256_finish(&state, &result);
    memcpy(hash_out, result.bytes, 32);
}

static void gen_random(uint8_t *out, uint8_t len) {
    for (uint8_t i = 0; i < len; i += 4) {
        uint32_t r = get_rand_32();
        uint8_t remaining = len - i;
        if (remaining > 4) remaining = 4;
        memcpy(out + i, &r, remaining);
    }
}

static bool hex_decode_byte(char hi, char lo, uint8_t *out) {
    uint8_t v = 0;
    if (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
    else if (hi >= 'a' && hi <= 'f') v = (hi - 'a' + 10) << 4;
    else if (hi >= 'A' && hi <= 'F') v = (hi - 'A' + 10) << 4;
    else return false;
    if (lo >= '0' && lo <= '9') v |= lo - '0';
    else if (lo >= 'a' && lo <= 'f') v |= lo - 'a' + 10;
    else if (lo >= 'A' && lo <= 'F') v |= lo - 'A' + 10;
    else return false;
    *out = v;
    return true;
}

// ============================================================
// Card encoding / decoding
// ============================================================

// Build a user card in binary format.
// Returns total length written to buf.
static uint16_t build_user_card(uint8_t *buf, uint16_t buf_size,
                                const char *username, uint8_t ulen,
                                const uint8_t *hash, const uint8_t *salt,
                                uint8_t flags, uint8_t fail_count,
                                const uint16_t *rpacks, uint8_t rcount,
                                const uint16_t *wpacks, uint8_t wcount,
                                const uint16_t *dpacks, uint8_t dcount) {
    uint16_t off = 0;

    // Header: magic + version
    if (off + 4 > buf_size) return 0;
    buf[off++] = CARD_MAGIC_LO;
    buf[off++] = CARD_MAGIC_HI;
    buf[off++] = 1;  // version lo
    buf[off++] = 0;  // version hi

    // Field 0: username (ascii, length-prefixed in data)
    if (off + 2 + 1 + ulen > buf_size) return 0;
    buf[off++] = 0;           // ord 0, no flags
    buf[off++] = 1 + ulen;   // length = prefix byte + chars
    buf[off++] = ulen;        // length prefix
    memcpy(buf + off, username, ulen);
    off += ulen;

    // Field 1: password hash (32 bytes)
    if (off + 2 + USER_HASH_LEN > buf_size) return 0;
    buf[off++] = 1;               // ord 1
    buf[off++] = USER_HASH_LEN;
    memcpy(buf + off, hash, USER_HASH_LEN);
    off += USER_HASH_LEN;

    // Field 2: salt (16 bytes)
    if (off + 2 + USER_SALT_LEN > buf_size) return 0;
    buf[off++] = 2;               // ord 2
    buf[off++] = USER_SALT_LEN;
    memcpy(buf + off, salt, USER_SALT_LEN);
    off += USER_SALT_LEN;

    // Field 3: flags (uint8)
    if (off + 3 > buf_size) return 0;
    buf[off++] = 3;
    buf[off++] = 1;
    buf[off++] = flags;

    // Field 4: fail_count (uint8)
    if (off + 3 > buf_size) return 0;
    buf[off++] = 4;
    buf[off++] = 1;
    buf[off++] = fail_count;

    // Field 5: readPacks (array_u16: length-prefixed)
    if (off + 2 + 1 + rcount * 2 > buf_size) return 0;
    buf[off++] = 5;
    buf[off++] = 1 + rcount * 2;
    buf[off++] = rcount * 2;  // byte count of array data
    for (uint8_t i = 0; i < rcount; i++) {
        buf[off++] = rpacks[i] & 0xFF;
        buf[off++] = (rpacks[i] >> 8) & 0xFF;
    }

    // Field 6: writePacks
    if (off + 2 + 1 + wcount * 2 > buf_size) return 0;
    buf[off++] = 6;
    buf[off++] = 1 + wcount * 2;
    buf[off++] = wcount * 2;
    for (uint8_t i = 0; i < wcount; i++) {
        buf[off++] = wpacks[i] & 0xFF;
        buf[off++] = (wpacks[i] >> 8) & 0xFF;
    }

    // Field 7: deletePacks
    if (off + 2 + 1 + dcount * 2 > buf_size) return 0;
    buf[off++] = 7;
    buf[off++] = 1 + dcount * 2;
    buf[off++] = dcount * 2;
    for (uint8_t i = 0; i < dcount; i++) {
        buf[off++] = dpacks[i] & 0xFF;
        buf[off++] = (dpacks[i] >> 8) & 0xFF;
    }

    return off;
}

// ============================================================
// Schema card builder — writes a Pack 0 card describing fields
//
// The client JS (picowal.js loadMeta) reads Pack 0 cards:
//   Ord 0: pack name (ascii, length-prefixed)
//   Ord 1: field count (uint8)
//   Ord 2: field defs — 3 bytes each: [ord_byte][type_code][maxlen]
//   Ord 5: field names — null-separated ASCII strings
// ============================================================

typedef struct {
    uint8_t  ord;
    uint8_t  type;     // FT_* constant
    uint8_t  maxlen;
    const char *name;
} schema_field_t;

static uint16_t build_schema_card_full(uint8_t *buf, uint16_t buf_size,
                                      const char *pack_name,
                                      const schema_field_t *fields,
                                      uint8_t field_count,
                                      uint8_t flags,
                                      const char *module) {
    uint16_t off = 0;

    // Header: magic + version
    if (off + 4 > buf_size) return 0;
    buf[off++] = CARD_MAGIC_LO;
    buf[off++] = CARD_MAGIC_HI;
    buf[off++] = 1;  // version lo
    buf[off++] = 0;  // version hi

    // Ord 0: pack name (ascii, length-prefixed)
    uint8_t nlen = (uint8_t)strlen(pack_name);
    if (off + 2 + 1 + nlen > buf_size) return 0;
    buf[off++] = 0;           // ord 0
    buf[off++] = 1 + nlen;   // field length
    buf[off++] = nlen;        // string length prefix
    memcpy(buf + off, pack_name, nlen);
    off += nlen;

    // Ord 1: field count (uint8)
    if (off + 3 > buf_size) return 0;
    buf[off++] = 1;             // ord 1
    buf[off++] = 1;             // field length
    buf[off++] = field_count;

    // Ord 2: field definitions — 3 bytes each
    uint16_t defs_len = (uint16_t)field_count * 3;
    if (off + 2 + defs_len > buf_size) return 0;
    buf[off++] = 2;             // ord 2
    buf[off++] = (uint8_t)defs_len;
    for (uint8_t i = 0; i < field_count; i++) {
        buf[off++] = fields[i].ord & 0x1F;
        buf[off++] = fields[i].type;
        buf[off++] = fields[i].maxlen;
    }

    // Ord 3: pack flags (uint8) — bit 0 = public-read
    if (off + 3 > buf_size) return 0;
    buf[off++] = 3;
    buf[off++] = 1;
    buf[off++] = flags;

    // Ord 4: module name (ascii, length-prefixed) — groups packs in nav
    if (module && module[0]) {
        uint8_t mlen = (uint8_t)strlen(module);
        if (off + 2 + 1 + mlen <= buf_size) {
            buf[off++] = 4;
            buf[off++] = 1 + mlen;
            buf[off++] = mlen;
            memcpy(buf + off, module, mlen);
            off += mlen;
        }
    }

    // Ord 5: field names — null-separated ASCII
    uint16_t names_total = 0;
    for (uint8_t i = 0; i < field_count; i++) {
        names_total += (uint16_t)strlen(fields[i].name) + 1;
    }
    if (off + 2 + names_total > buf_size || names_total > 255) return 0;
    buf[off++] = 5;
    buf[off++] = (uint8_t)names_total;
    for (uint8_t i = 0; i < field_count; i++) {
        uint8_t slen = (uint8_t)strlen(fields[i].name);
        memcpy(buf + off, fields[i].name, slen);
        off += slen;
        buf[off++] = '\0';
    }

    return off;
}

static uint16_t build_schema_card_ex(uint8_t *buf, uint16_t buf_size,
                                      const char *pack_name,
                                      const schema_field_t *fields,
                                      uint8_t field_count,
                                      uint8_t flags) {
    return build_schema_card_full(buf, buf_size, pack_name, fields, field_count, flags, NULL);
}

static uint16_t build_schema_card(uint8_t *buf, uint16_t buf_size,
                                   const char *pack_name,
                                   const schema_field_t *fields,
                                   uint8_t field_count) {
    return build_schema_card_ex(buf, buf_size, pack_name, fields, field_count, 0);
}

// Seed a schema card into Pack 0 for the given pack number.
static bool seed_schema(uint16_t pack_ord, const char *pack_name,
                         const schema_field_t *fields, uint8_t field_count) {
    uint8_t card[256];
    uint16_t clen = build_schema_card(card, sizeof(card),
                                       pack_name, fields, field_count);
    if (clen == 0) return false;
    return kv_put(SCHEMA_KEY(pack_ord), card, clen);
}

#define SCHEMA_FLAG_PUBLIC_READ 0x01

static bool seed_schema_public(uint16_t pack_ord, const char *pack_name,
                                const schema_field_t *fields, uint8_t field_count) {
    uint8_t card[256];
    uint16_t clen = build_schema_card_full(card, sizeof(card),
                                          pack_name, fields, field_count,
                                          SCHEMA_FLAG_PUBLIC_READ, "Reference");
    if (clen == 0) return false;
    return kv_put(SCHEMA_KEY(pack_ord), card, clen);
}

// Public API version — same layout, just different type name
bool user_auth_seed_schema(uint16_t pack_ord, const char *pack_name,
                           const user_auth_schema_field_t *fields,
                           uint8_t field_count) {
    // user_auth_schema_field_t and schema_field_t have identical layout
    return seed_schema(pack_ord, pack_name,
                       (const schema_field_t *)fields, field_count);
}

bool user_auth_seed_schema_module(uint16_t pack_ord, const char *pack_name,
                                   const user_auth_schema_field_t *fields,
                                   uint8_t field_count, const char *module) {
    uint8_t card[256];
    uint16_t clen = build_schema_card_full(card, sizeof(card),
                                           pack_name,
                                           (const schema_field_t *)fields,
                                           field_count, 0, module);
    if (clen == 0) return false;
    return kv_put(SCHEMA_KEY(pack_ord), card, clen);
}

// Parse a field from a card at the given offset.
// Returns the data pointer and length, advances *off.
static bool parse_field(const uint8_t *card, uint16_t card_len,
                        uint16_t *off, uint8_t *ord, uint8_t *flen,
                        const uint8_t **data) {
    if (*off + 2 > card_len) return false;
    *ord = card[*off] & 0x1F;
    *flen = card[(*off) + 1];
    *off += 2;
    if (*off + *flen > card_len) return false;
    *data = card + *off;
    *off += *flen;
    return true;
}

// Extract user fields from a card buffer.
typedef struct {
    const uint8_t *username;  uint8_t ulen;
    const uint8_t *hash;
    const uint8_t *salt;
    uint8_t flags;
    uint8_t fail_count;
    const uint8_t *read_data;  uint8_t read_bytes;
    const uint8_t *write_data; uint8_t write_bytes;
    const uint8_t *del_data;   uint8_t del_bytes;
} parsed_user_t;

static bool parse_user_card(const uint8_t *card, uint16_t card_len,
                            parsed_user_t *out) {
    memset(out, 0, sizeof(*out));
    if (card_len < 4) return false;
    if (card[0] != CARD_MAGIC_LO || card[1] != CARD_MAGIC_HI) return false;

    uint16_t off = 4;  // skip magic + version
    while (off < card_len) {
        uint8_t ord, flen;
        const uint8_t *data;
        if (!parse_field(card, card_len, &off, &ord, &flen, &data)) break;

        switch (ord) {
            case 0:  // username (length-prefixed ascii)
                if (flen >= 1) {
                    out->ulen = data[0];
                    out->username = data + 1;
                    if (out->ulen > flen - 1) out->ulen = flen - 1;
                }
                break;
            case 1: out->hash = data; break;
            case 2: out->salt = data; break;
            case 3: out->flags = (flen >= 1) ? data[0] : 0; break;
            case 4: out->fail_count = (flen >= 1) ? data[0] : 0; break;
            case 5:  // readPacks (length-prefixed array)
                if (flen >= 1) {
                    out->read_bytes = data[0];
                    out->read_data = data + 1;
                    if (out->read_bytes > flen - 1) out->read_bytes = flen - 1;
                }
                break;
            case 6:
                if (flen >= 1) {
                    out->write_bytes = data[0];
                    out->write_data = data + 1;
                    if (out->write_bytes > flen - 1) out->write_bytes = flen - 1;
                }
                break;
            case 7:
                if (flen >= 1) {
                    out->del_bytes = data[0];
                    out->del_data = data + 1;
                    if (out->del_bytes > flen - 1) out->del_bytes = flen - 1;
                }
                break;
        }
    }
    return out->username != NULL && out->hash != NULL && out->salt != NULL;
}

static void extract_packs(const uint8_t *data, uint8_t bytes,
                          uint16_t *out, uint8_t *count, uint8_t max) {
    *count = 0;
    if (!data) return;
    for (uint8_t i = 0; i + 1 < bytes && *count < max; i += 2) {
        out[*count] = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        (*count)++;
    }
}

// ============================================================
// Find user by username — scan Pack 1
// ============================================================

static int32_t find_user_by_name(const char *name, uint8_t nlen,
                                 uint8_t *card_buf, uint16_t buf_size,
                                 uint16_t *card_len_out) {
    uint32_t keys[256];
    uint32_t count = kv_range(USER_KEY(0), 0xFFC00000u, keys, NULL, 256);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t card_id = keys[i] & 0x3FFFFF;
        uint16_t len = buf_size;
        if (!kv_get_copy(keys[i], card_buf, &len, NULL)) continue;

        parsed_user_t user;
        if (!parse_user_card(card_buf, len, &user)) continue;

        if (user.ulen == nlen && memcmp(user.username, name, nlen) == 0) {
            if (card_len_out) *card_len_out = len;
            return (int32_t)card_id;
        }
    }
    return -1;
}

// ============================================================
// Public API
// ============================================================

void user_auth_init(void) {
    memset(g_sessions, 0, sizeof(g_sessions));

    // Always ensure Pack 1 schema exists in Pack 0
    // (idempotent — overwrites if already present)
    static const schema_field_t user_fields[] = {
        { 0, FT_UTF8,      31, "username" },
        { 1, FT_BLOB,      32, "password_hash" },
        { 2, FT_BLOB,      16, "salt" },
        { 3, FT_UINT8,      1, "flags" },
        { 4, FT_UINT8,      1, "fail_count" },
        { 5, FT_ARRAY_U16, 40, "readPacks" },
        { 6, FT_ARRAY_U16, 40, "writePacks" },
        { 7, FT_ARRAY_U16, 40, "deletePacks" },
    };
    if (seed_schema(USER_PACK, "users", user_fields, 8)) {
        printf("[auth] Pack 1 (users) schema seeded in Pack 0\n");
    }

    // Seed reference data packs (idempotent — only if schema card missing)
    // Pack 2: Days of Week
    {
        static const schema_field_t fields[] = {
            { 0, FT_UTF8, 12, "name" },
            { 1, FT_UTF8,   3, "abbr" },
        };
        if (!kv_exists(SCHEMA_KEY(2))) {
            seed_schema_public(2, "days", fields, 2);
            static const char *names[] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
            static const char *abbrs[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
            for (int i = 0; i < 7; i++) {
                uint8_t c[64]; uint16_t o = 0;
                c[o++]=0x7D; c[o++]=0xCA; c[o++]=1; c[o++]=0;
                uint8_t nl=(uint8_t)strlen(names[i]); c[o++]=0; c[o++]=1+nl; c[o++]=nl; memcpy(c+o,names[i],nl); o+=nl;
                uint8_t al=(uint8_t)strlen(abbrs[i]); c[o++]=1; c[o++]=1+al; c[o++]=al; memcpy(c+o,abbrs[i],al); o+=al;
                kv_put(PACK_KEY(2, i), c, o);
            }
            printf("[auth] Pack 2 (days) seeded — 7 cards\n");
        }
    }
    // Pack 3: Countries
    {
        static const schema_field_t fields[] = {
            { 0, FT_UTF8, 48, "name" },
            { 1, FT_UTF8,   2, "code" },
        };
        if (!kv_exists(SCHEMA_KEY(3))) {
            seed_schema_public(3, "countries", fields, 2);
            static const char *names[] = {"United Kingdom","United States","Germany","France","Japan",
                "Canada","Australia","Italy","Spain","Netherlands","Switzerland","Sweden"};
            static const char *codes[] = {"GB","US","DE","FR","JP","CA","AU","IT","ES","NL","CH","SE"};
            for (int i = 0; i < 12; i++) {
                uint8_t c[80]; uint16_t o = 0;
                c[o++]=0x7D; c[o++]=0xCA; c[o++]=1; c[o++]=0;
                uint8_t nl=(uint8_t)strlen(names[i]); c[o++]=0; c[o++]=1+nl; c[o++]=nl; memcpy(c+o,names[i],nl); o+=nl;
                uint8_t cl=(uint8_t)strlen(codes[i]); c[o++]=1; c[o++]=1+cl; c[o++]=cl; memcpy(c+o,codes[i],cl); o+=cl;
                kv_put(PACK_KEY(3, i), c, o);
            }
            printf("[auth] Pack 3 (countries) seeded — 12 cards\n");
        }
    }
    // Pack 4: Currencies
    {
        static const schema_field_t fields[] = {
            { 0, FT_UTF8, 32, "name" },
            { 1, FT_UTF8,   3, "code" },
        };
        if (!kv_exists(SCHEMA_KEY(4))) {
            seed_schema_public(4, "currencies", fields, 2);
            static const char *names[] = {"British Pound","US Dollar","Euro","Japanese Yen","Swiss Franc",
                "Canadian Dollar","Australian Dollar","Chinese Yuan","Indian Rupee","Brazilian Real",
                "South Korean Won","Swedish Krona","Norwegian Krone","Danish Krone","Singapore Dollar"};
            static const char *codes[] = {"GBP","USD","EUR","JPY","CHF","CAD","AUD","CNY","INR","BRL","KRW","SEK","NOK","DKK","SGD"};
            for (int i = 0; i < 15; i++) {
                uint8_t c[64]; uint16_t o = 0;
                c[o++]=0x7D; c[o++]=0xCA; c[o++]=1; c[o++]=0;
                uint8_t nl=(uint8_t)strlen(names[i]); c[o++]=0; c[o++]=1+nl; c[o++]=nl; memcpy(c+o,names[i],nl); o+=nl;
                uint8_t cl=(uint8_t)strlen(codes[i]); c[o++]=1; c[o++]=1+cl; c[o++]=cl; memcpy(c+o,codes[i],cl); o+=cl;
                kv_put(PACK_KEY(4, i), c, o);
            }
            printf("[auth] Pack 4 (currencies) seeded — 15 cards\n");
        }
    }

    // Check if admin user exists in Pack 1, Card 0
    uint16_t len = 512;
    uint8_t buf[512];
    if (kv_get_copy(USER_KEY(0), buf, &len, NULL)) {
        printf("[auth] Admin user found\n");
        return;
    }

    // First boot: create admin user
    printf("[auth] First boot — creating admin user\n");

    uint8_t salt[USER_SALT_LEN];
    gen_random(salt, USER_SALT_LEN);

    uint8_t hash[USER_HASH_LEN];
    sha256_hash(salt, USER_SALT_LEN, "admin", 5, hash);

    uint16_t wildcard = 0xFFFF;
    uint8_t card[256];
    uint16_t clen = build_user_card(card, sizeof(card),
                                     "admin", 5,
                                     hash, salt,
                                     USER_FLAG_ADMIN, 0,
                                     &wildcard, 1,
                                     &wildcard, 1,
                                     &wildcard, 1);
    if (clen > 0 && kv_put(USER_KEY(0), card, clen)) {
        printf("[auth] Admin user created (card 0, password: admin)\n");
    } else {
        printf("[auth] ERROR: failed to create admin user\n");
    }
}

int32_t user_auth_login(const char *username, uint8_t ulen,
                        const char *password, uint8_t plen,
                        uint8_t token_out[SESSION_TOKEN_LEN]) {
    if (ulen == 0 || ulen > USER_MAX_NAME) return -1;
    if (plen == 0 || plen > USER_MAX_PASS) return -1;

    uint8_t card_buf[512];
    uint16_t card_len = 0;
    int32_t card_id = find_user_by_name(username, ulen, card_buf, sizeof(card_buf), &card_len);
    if (card_id < 0) return -1;

    parsed_user_t user;
    if (!parse_user_card(card_buf, card_len, &user)) return -1;

    // Check locked
    if (user.flags & USER_FLAG_LOCKED) return -1;

    // Verify password
    uint8_t computed[USER_HASH_LEN];
    sha256_hash(user.salt, USER_SALT_LEN, password, plen, computed);

    uint8_t diff = 0;
    for (int i = 0; i < USER_HASH_LEN; i++) diff |= computed[i] ^ user.hash[i];
    if (diff != 0) return -1;

    // Find a free session slot (or expire oldest)
    int slot = -1;
    uint32_t oldest_ms = UINT32_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < SESSION_MAX; i++) {
        if (!g_sessions[i].active) { slot = i; break; }
        if (g_sessions[i].created_ms < oldest_ms) {
            oldest_ms = g_sessions[i].created_ms;
            oldest_slot = i;
        }
    }
    if (slot < 0) {
        // Expire oldest
        slot = oldest_slot;
        g_sessions[slot].active = false;
    }

    // Create session
    gen_random(g_sessions[slot].token, SESSION_TOKEN_LEN);
    g_sessions[slot].user_card = (uint32_t)card_id;
    g_sessions[slot].created_ms = now_ms();
    g_sessions[slot].last_ms = now_ms();
    g_sessions[slot].active = true;

    memcpy(token_out, g_sessions[slot].token, SESSION_TOKEN_LEN);
    printf("[auth] Login: %.*s → card %ld, session slot %d\n",
           (int)ulen, username, (long)card_id, slot);
    return card_id;
}

void user_auth_logout(const uint8_t token[SESSION_TOKEN_LEN]) {
    for (int i = 0; i < SESSION_MAX; i++) {
        if (g_sessions[i].active &&
            memcmp(g_sessions[i].token, token, SESSION_TOKEN_LEN) == 0) {
            g_sessions[i].active = false;
            printf("[auth] Logout: session slot %d\n", i);
            return;
        }
    }
}

bool user_auth_check(const uint8_t token[SESSION_TOKEN_LEN],
                     user_session_t *out) {
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!g_sessions[i].active) continue;
        if (memcmp(g_sessions[i].token, token, SESSION_TOKEN_LEN) != 0) continue;

        // Check timeout
        uint32_t elapsed = now_ms() - g_sessions[i].last_ms;
        if (elapsed > (uint32_t)SESSION_TIMEOUT_S * 1000u) {
            g_sessions[i].active = false;
            return false;
        }

        // Refresh last-active
        g_sessions[i].last_ms = now_ms();

        // Load user card to get current ACLs
        uint8_t card_buf[512];
        uint16_t card_len = sizeof(card_buf);
        if (!kv_get_copy(USER_KEY(g_sessions[i].user_card),
                         card_buf, &card_len, NULL)) {
            g_sessions[i].active = false;
            return false;
        }

        parsed_user_t user;
        if (!parse_user_card(card_buf, card_len, &user)) return false;

        out->user_card = g_sessions[i].user_card;
        out->flags = user.flags;
        extract_packs(user.read_data, user.read_bytes,
                      out->read_packs, &out->read_count, 20);
        extract_packs(user.write_data, user.write_bytes,
                      out->write_packs, &out->write_count, 20);
        extract_packs(user.del_data, user.del_bytes,
                      out->delete_packs, &out->delete_count, 20);
        return true;
    }
    return false;
}

static bool pack_in_list(const uint16_t *list, uint8_t count, uint16_t pack) {
    for (uint8_t i = 0; i < count; i++) {
        if (list[i] == 0xFFFF || list[i] == pack) return true;
    }
    return false;
}

// Check if a pack's schema card has the public-read flag (ord 3, bit 0)
static bool pack_is_public_read(uint16_t pack) {
    uint8_t sbuf[128]; uint16_t slen = sizeof(sbuf);
    if (!kv_get_copy(SCHEMA_KEY(pack), sbuf, &slen, NULL)) return false;
    if (slen < 4 || sbuf[0] != CARD_MAGIC_LO || sbuf[1] != CARD_MAGIC_HI) return false;
    uint16_t off = 4;
    while (off + 1 < slen) {
        uint8_t ord = sbuf[off] & 0x1F;
        uint8_t flen = sbuf[off + 1];
        off += 2;
        if (off + flen > slen) break;
        if (ord == 3 && flen >= 1) return (sbuf[off] & SCHEMA_FLAG_PUBLIC_READ) != 0;
        off += flen;
    }
    return false;
}

bool user_auth_can_read(const user_session_t *s, uint16_t pack) {
    if (pack_in_list(s->read_packs, s->read_count, pack)) return true;
    return pack_is_public_read(pack);
}

bool user_auth_can_write(const user_session_t *s, uint16_t pack) {
    return pack_in_list(s->write_packs, s->write_count, pack);
}

bool user_auth_can_delete(const user_session_t *s, uint16_t pack) {
    return pack_in_list(s->delete_packs, s->delete_count, pack);
}

bool user_auth_is_admin(const user_session_t *s) {
    return (s->flags & USER_FLAG_ADMIN) != 0;
}

bool user_auth_change_password(uint32_t user_card,
                               const char *old_pass, uint8_t old_len,
                               const char *new_pass, uint8_t new_len) {
    uint8_t card_buf[512];
    uint16_t card_len = sizeof(card_buf);
    if (!kv_get_copy(USER_KEY(user_card), card_buf, &card_len, NULL))
        return false;

    parsed_user_t user;
    if (!parse_user_card(card_buf, card_len, &user)) return false;

    // Verify old password
    uint8_t computed[USER_HASH_LEN];
    sha256_hash(user.salt, USER_SALT_LEN, old_pass, old_len, computed);
    uint8_t diff = 0;
    for (int i = 0; i < USER_HASH_LEN; i++) diff |= computed[i] ^ user.hash[i];
    if (diff != 0) return false;

    // Build new card with new password
    uint8_t new_salt[USER_SALT_LEN];
    gen_random(new_salt, USER_SALT_LEN);

    uint8_t new_hash[USER_HASH_LEN];
    sha256_hash(new_salt, USER_SALT_LEN, new_pass, new_len, new_hash);

    // Extract existing pack ACLs
    uint16_t rpacks[20], wpacks[20], dpacks[20];
    uint8_t rc, wc, dc;
    extract_packs(user.read_data, user.read_bytes, rpacks, &rc, 20);
    extract_packs(user.write_data, user.write_bytes, wpacks, &wc, 20);
    extract_packs(user.del_data, user.del_bytes, dpacks, &dc, 20);

    // Extract username
    char uname[USER_MAX_NAME + 1];
    uint8_t ulen = user.ulen;
    if (ulen > USER_MAX_NAME) ulen = USER_MAX_NAME;
    memcpy(uname, user.username, ulen);

    uint8_t new_card[256];
    uint16_t new_len_card = build_user_card(new_card, sizeof(new_card),
                                             uname, ulen,
                                             new_hash, new_salt,
                                             user.flags, 0,
                                             rpacks, rc, wpacks, wc, dpacks, dc);
    if (new_len_card == 0) return false;

    return kv_put(USER_KEY(user_card), new_card, new_len_card);
}

int32_t user_auth_create_user(const char *username, uint8_t ulen,
                              const char *password, uint8_t plen,
                              uint8_t flags,
                              const uint16_t *read_packs, uint8_t rcount,
                              const uint16_t *write_packs, uint8_t wcount,
                              const uint16_t *delete_packs, uint8_t dcount) {
    if (ulen == 0 || ulen > USER_MAX_NAME) return -1;
    if (plen == 0 || plen > USER_MAX_PASS) return -1;

    // Check username not taken
    uint8_t tmp[512];
    if (find_user_by_name(username, ulen, tmp, sizeof(tmp), NULL) >= 0)
        return -1;

    // Find next free card ID (scan existing)
    uint32_t keys[256];
    uint32_t count = kv_range(USER_KEY(0), 0xFFC00000u, keys, NULL, 256);
    uint32_t next_id = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = keys[i] & 0x3FFFFF;
        if (id >= next_id) next_id = id + 1;
    }

    uint8_t salt[USER_SALT_LEN];
    gen_random(salt, USER_SALT_LEN);

    uint8_t hash[USER_HASH_LEN];
    sha256_hash(salt, USER_SALT_LEN, password, plen, hash);

    uint8_t card[256];
    uint16_t clen = build_user_card(card, sizeof(card),
                                     username, ulen,
                                     hash, salt,
                                     flags, 0,
                                     read_packs, rcount,
                                     write_packs, wcount,
                                     delete_packs, dcount);
    if (clen == 0) return -1;

    if (!kv_put(USER_KEY(next_id), card, clen)) return -1;

    printf("[auth] Created user '%.*s' → card %lu\n",
           (int)ulen, username, (unsigned long)next_id);
    return (int32_t)next_id;
}

bool user_auth_parse_cookie(const char *headers,
                            uint8_t token_out[SESSION_TOKEN_LEN]) {
    const char *p = strstr(headers, "Cookie:");
    if (!p) p = strstr(headers, "cookie:");
    if (!p) return false;

    const char *sid = strstr(p, "sid=");
    if (!sid) return false;
    sid += 4;

    // Need 32 hex chars
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        if (!hex_decode_byte(sid[i * 2], sid[i * 2 + 1], &token_out[i]))
            return false;
    }
    return true;
}

void user_auth_format_token(const uint8_t token[SESSION_TOKEN_LEN],
                            char hex_out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        hex_out[i * 2] = hex[token[i] >> 4];
        hex_out[i * 2 + 1] = hex[token[i] & 0x0F];
    }
    hex_out[32] = '\0';
}
