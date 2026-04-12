#ifndef USER_AUTH_H
#define USER_AUTH_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// User Auth — in-memory sessions + Pack 1 user records
//
// User cards live in KV Pack 1. Each card stores:
//   Field 0 (ord 0): username    — ascii, length-prefixed
//   Field 1 (ord 1): pass_hash   — 32 bytes SHA-256(salt || password)
//   Field 2 (ord 2): salt        — 16 bytes random
//   Field 3 (ord 3): flags       — uint8 (bit0=admin, bit1=locked)
//   Field 4 (ord 4): fail_count  — uint8
//   Field 5 (ord 5): readPacks   — array_u16 (0xFFFF = wildcard)
//   Field 6 (ord 6): writePacks  — array_u16
//   Field 7 (ord 7): deletePacks — array_u16
//
// Sessions are in-memory only (lost on reboot).
// ============================================================

#define USER_PACK           1
#define USER_MAX_NAME       31
#define USER_MAX_PASS       31
#define USER_SALT_LEN       16
#define USER_HASH_LEN       32
#define SESSION_TOKEN_LEN   16
#define SESSION_MAX         8
#define SESSION_TIMEOUT_S   3600

#define USER_FLAG_ADMIN     0x01
#define USER_FLAG_LOCKED    0x02

// Session info returned by check
typedef struct {
    uint32_t user_card;
    uint8_t  flags;
    uint16_t read_packs[20];
    uint8_t  read_count;
    uint16_t write_packs[20];
    uint8_t  write_count;
    uint16_t delete_packs[20];
    uint8_t  delete_count;
} user_session_t;

// Initialize auth subsystem. Seeds admin user on first boot.
void user_auth_init(void);

// Login: returns user_card on success, -1 on failure.
// On success, token_out is filled with SESSION_TOKEN_LEN bytes.
int32_t user_auth_login(const char *username, uint8_t ulen,
                        const char *password, uint8_t plen,
                        uint8_t token_out[SESSION_TOKEN_LEN]);

// Logout: expire session matching token.
void user_auth_logout(const uint8_t token[SESSION_TOKEN_LEN]);

// Check session: look up token, return session info.
// Returns true if session is valid.
bool user_auth_check(const uint8_t token[SESSION_TOKEN_LEN],
                     user_session_t *out);

// Check if session has read/write/delete access to a pack.
bool user_auth_can_read(const user_session_t *s, uint16_t pack);
bool user_auth_can_write(const user_session_t *s, uint16_t pack);
bool user_auth_can_delete(const user_session_t *s, uint16_t pack);
bool user_auth_is_admin(const user_session_t *s);

// Change password for a user card. Verifies old password first.
bool user_auth_change_password(uint32_t user_card,
                               const char *old_pass, uint8_t old_len,
                               const char *new_pass, uint8_t new_len);

// Create a new user. Returns the card ID, or -1 on failure.
// Caller must be admin. packs arrays can be NULL if count is 0.
int32_t user_auth_create_user(const char *username, uint8_t ulen,
                              const char *password, uint8_t plen,
                              uint8_t flags,
                              const uint16_t *read_packs, uint8_t rcount,
                              const uint16_t *write_packs, uint8_t wcount,
                              const uint16_t *delete_packs, uint8_t dcount);

// Parse cookie header, extract session token.
// Looks for "sid=" followed by 32 hex chars.
// Returns true if found and decoded.
bool user_auth_parse_cookie(const char *headers,
                            uint8_t token_out[SESSION_TOKEN_LEN]);

// Format token as 32-char hex string (needs 33 bytes for null).
void user_auth_format_token(const uint8_t token[SESSION_TOKEN_LEN],
                            char hex_out[33]);

// Schema field definition for seeding Pack 0 metadata cards.
typedef struct {
    uint8_t     ord;
    uint8_t     type;     // FT_* type code matching picowal.js T.*
    uint8_t     maxlen;
    const char *name;
} user_auth_schema_field_t;

// Seed a schema card into Pack 0 for the given pack.
// Idempotent — can be called on every boot.
bool user_auth_seed_schema(uint16_t pack_ord, const char *pack_name,
                           const user_auth_schema_field_t *fields,
                           uint8_t field_count);
bool user_auth_seed_schema_module(uint16_t pack_ord, const char *pack_name,
                                   const user_auth_schema_field_t *fields,
                                   uint8_t field_count, const char *module);

// Field type codes matching picowal.js T.* constants
#define UA_FT_UINT8      0x01
#define UA_FT_UINT16     0x02
#define UA_FT_UINT32     0x03
#define UA_FT_BOOL       0x07
#define UA_FT_ASCII      0x08
#define UA_FT_UTF8       0x09
#define UA_FT_ARRAY_U16  0x10
#define UA_FT_ARRAY_U16  0x10
#define UA_FT_BLOB       0x11
#define UA_FT_LOOKUP     0x12

// Schema card flags (ord 3 in Pack 0 schema cards)
#define UA_SCHEMA_FLAG_PUBLIC_READ 0x01

#endif
