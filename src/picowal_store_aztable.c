#define _GNU_SOURCE

#include "picowal_store_aztable.h"

#if defined(_WIN32)
#error "picowal_store_aztable is a POSIX/Linux host backend; do not compile it into Windows or Pico firmware targets."
#endif

#include "picowal_api.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define AZTABLE_RESPONSE_MAX 1048576u
#define AZTABLE_COMMAND_MAX  4096u

static bool az_ready(void *ctx) {
    return ctx && ((picowal_aztable_store_t *)ctx)->ready;
}

static bool copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0 || !src) return false;
    size_t len = strlen(src);
    if (len == 0 || len >= dst_len) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool shell_quote(const char *src, char *dst, size_t dst_len) {
    size_t off = 0;
    if (dst_len < 3) return false;
    dst[off++] = '\'';
    for (; *src; src++) {
        if (*src == '\'') {
            if (off + 4 >= dst_len) return false;
            memcpy(dst + off, "'\\''", 4);
            off += 4;
        } else {
            if (off + 1 >= dst_len) return false;
            dst[off++] = *src;
        }
    }
    if (off + 2 > dst_len) return false;
    dst[off++] = '\'';
    dst[off] = '\0';
    return true;
}

static bool az_url(const picowal_aztable_store_t *ctx, const char *entity,
                   const char *query, char *url, size_t url_len) {
    const char *sas = ctx->sas;
    if (*sas == '?') sas++;
    bool has_query = query && *query;
    bool has_sas = *sas != '\0';
    int n = snprintf(url, url_len, "%s%s%s%s%s%s",
                     ctx->endpoint,
                     entity ? entity : "",
                     (has_query || has_sas) ? "?" : "",
                     has_query ? query : "",
                     (has_query && has_sas) ? "&" : "",
                     sas);
    return n > 0 && (size_t)n < url_len;
}

static bool write_temp_body(const char *body, char *path, size_t path_len) {
    int n = snprintf(path, path_len, "/tmp/picowal-aztable-body-XXXXXX");
    if (n <= 0 || (size_t)n >= path_len) return false;
    int fd = mkstemp(path);
    if (fd < 0) return false;
    size_t len = strlen(body);
    const char *p = body;
    bool ok = true;
    while (len > 0) {
        ssize_t wrote = write(fd, p, len);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (wrote == 0) {
            ok = false;
            break;
        }
        p += wrote;
        len -= (size_t)wrote;
    }
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

static bool az_request(picowal_aztable_store_t *ctx, const char *method,
                       const char *entity, const char *query, const char *body,
                       char *response, size_t response_len) {
    char url[1024], qurl[1400], qbody[512], body_path[128], cmd[AZTABLE_COMMAND_MAX];
    body_path[0] = '\0';
    if (!az_url(ctx, entity, query, url, sizeof(url))) return false;
    if (!shell_quote(url, qurl, sizeof(qurl))) return false;
    if (body && !write_temp_body(body, body_path, sizeof(body_path))) return false;

    const char *body_arg = "";
    if (body) {
        if (!shell_quote(body_path, qbody, sizeof(qbody))) {
            unlink(body_path);
            return false;
        }
        body_arg = qbody;
    }

    const char *if_match = (strcmp(method, "PUT") == 0 || strcmp(method, "DELETE") == 0)
        ? "-H 'If-Match: *' "
        : "";
    int n = snprintf(cmd, sizeof(cmd),
        "curl -fsS -X %s "
        "-H 'x-ms-version: 2020-10-02' "
        "-H 'Accept: application/json;odata=nometadata' "
        "-H 'Content-Type: application/json' "
        "%s"
        "%s%s%s %s",
        method,
        if_match,
        body ? "--data-binary @" : "",
        body ? body_arg : "",
        body ? " " : "",
        qurl);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        if (body_path[0]) unlink(body_path);
        return false;
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        if (body_path[0]) unlink(body_path);
        return false;
    }
    size_t off = 0;
    if (response && response_len > 0) response[0] = '\0';
    if (response && response_len > 0) {
        while (off + 1 < response_len) {
            size_t got = fread(response + off, 1, response_len - off - 1, pipe);
            off += got;
            if (got == 0) break;
        }
        response[off] = '\0';
    } else {
        char discard[256];
        while (fread(discard, 1, sizeof(discard), pipe) > 0) {}
    }
    int status = pclose(pipe);
    if (body_path[0]) unlink(body_path);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool hex_encode(const uint8_t *value, uint16_t len, char *out, size_t out_len) {
    static const char digits[] = "0123456789abcdef";
    if (out_len < (size_t)len * 2u + 1u) return false;
    for (uint16_t i = 0; i < len; i++) {
        out[i * 2u] = digits[value[i] >> 4];
        out[i * 2u + 1u] = digits[value[i] & 0x0Fu];
    }
    out[(size_t)len * 2u] = '\0';
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const char *hex, uint8_t *out, uint16_t cap, uint16_t *out_len) {
    size_t len = strlen(hex);
    if ((len & 1u) != 0 || len / 2u > cap || len / 2u > PICOWAL_VALUE_MAX) return false;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i / 2u] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = (uint16_t)(len / 2u);
    return true;
}

static int32_t index_find(picowal_aztable_store_t *ctx, uint32_t key) {
    for (uint32_t i = 0; i < ctx->index_count; i++) {
        if (ctx->index[i].key == key) return (int32_t)i;
    }
    return -1;
}

static bool index_upsert(picowal_aztable_store_t *ctx, uint32_t key, uint16_t version) {
    int32_t pos = index_find(ctx, key);
    if (pos >= 0) {
        ctx->index[pos].version = version;
        return true;
    }
    if (ctx->index_count >= PICOWAL_AZTABLE_INDEX_MAX) return false;
    ctx->index[ctx->index_count++] = (picowal_aztable_index_entry_t){ .key = key, .version = version };
    return true;
}

static void index_delete(picowal_aztable_store_t *ctx, uint32_t key) {
    int32_t pos = index_find(ctx, key);
    if (pos < 0) return;
    ctx->index[(uint32_t)pos] = ctx->index[--ctx->index_count];
}

static bool json_string_after(const char *start, const char *name, char *out, size_t out_len) {
    const char *p = strstr(start, name);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    p++;
    size_t off = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (off + 1 >= out_len) return false;
        out[off++] = *p++;
    }
    out[off] = '\0';
    return *p == '"';
}

static bool json_uint_after(const char *start, const char *name, uint32_t *out) {
    const char *p = strstr(start, name);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return false;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_key_strings(const char *pk, const char *rk, uint32_t *key) {
    char *end = NULL;
    unsigned long pack = strtoul(pk, &end, 16);
    if (!end || *end || pack > PICOWAL_PACK_MAX) return false;
    end = NULL;
    unsigned long card = strtoul(rk, &end, 16);
    if (!end || *end || card > PICOWAL_CARD_MAX) return false;
    *key = picowal_api_key((uint16_t)pack, (uint32_t)card);
    return true;
}

static bool az_load_index(picowal_aztable_store_t *ctx) {
    char *response = (char *)malloc(AZTABLE_RESPONSE_MAX);
    if (!response) return false;
    bool ok = az_request(ctx, "GET", "", "$select=PartitionKey,RowKey,Version", NULL,
                         response, AZTABLE_RESPONSE_MAX);
    if (!ok) {
        free(response);
        return false;
    }
    ctx->index_count = 0;
    const char *p = response;
    while ((p = strstr(p, "\"PartitionKey\"")) != NULL) {
        char pk[8], rk[16];
        uint32_t key, version = 1;
        if (json_string_after(p, "\"PartitionKey\"", pk, sizeof(pk)) &&
            json_string_after(p, "\"RowKey\"", rk, sizeof(rk)) &&
            parse_key_strings(pk, rk, &key)) {
            json_uint_after(p, "\"Version\"", &version);
            if (!index_upsert(ctx, key, (uint16_t)version)) {
                free(response);
                return false;
            }
        }
        p += 14;
    }
    free(response);
    return true;
}

static bool az_entity(char *out, size_t out_len, uint32_t key) {
    int n = snprintf(out, out_len, "(PartitionKey='%03x',RowKey='%06lx')",
                     picowal_api_key_pack(key), (unsigned long)picowal_api_key_card(key));
    return n > 0 && (size_t)n < out_len;
}

static bool az_put(void *ctx, uint32_t key, const uint8_t *value, uint16_t len) {
    picowal_aztable_store_t *az = (picowal_aztable_store_t *)ctx;
    if (!az_ready(ctx) || len > PICOWAL_VALUE_MAX || (!value && len != 0)) return false;
    char value_hex[PICOWAL_VALUE_MAX * 2u + 1u];
    char body[1400], entity[80];
    if (!hex_encode(value, len, value_hex, sizeof(value_hex))) return false;
    uint16_t version = 1;
    int32_t pos = index_find(az, key);
    if (pos >= 0) version = (uint16_t)(az->index[pos].version + 1u);
    if (!az_entity(entity, sizeof(entity), key)) return false;
    int n = snprintf(body, sizeof(body),
                     "{\"PartitionKey\":\"%03x\",\"RowKey\":\"%06lx\",\"ValueHex\":\"%s\",\"Version\":%u}",
                     picowal_api_key_pack(key),
                     (unsigned long)picowal_api_key_card(key),
                     value_hex,
                     (unsigned)version);
    if (n <= 0 || (size_t)n >= sizeof(body)) return false;
    if (!az_request(az, pos >= 0 ? "PUT" : "POST", pos >= 0 ? entity : "", NULL, body, NULL, 0)) {
        return false;
    }
    return index_upsert(az, key, version);
}

static bool az_get_copy(void *ctx, uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    picowal_aztable_store_t *az = (picowal_aztable_store_t *)ctx;
    if (!az_ready(ctx) || !out || !len) return false;
    char entity[80];
    char *response = (char *)malloc(AZTABLE_RESPONSE_MAX);
    if (!response) return false;
    if (!az_entity(entity, sizeof(entity), key)) {
        free(response);
        return false;
    }
    bool ok = az_request(az, "GET", entity, "$select=ValueHex,Version", NULL,
                         response, AZTABLE_RESPONSE_MAX);
    if (!ok) {
        free(response);
        return false;
    }
    char value_hex[PICOWAL_VALUE_MAX * 2u + 1u];
    uint32_t parsed_version = 0;
    ok = json_string_after(response, "\"ValueHex\"", value_hex, sizeof(value_hex)) &&
         hex_decode(value_hex, out, *len, len);
    if (ok && json_uint_after(response, "\"Version\"", &parsed_version)) {
        if (version) *version = (uint16_t)parsed_version;
        index_upsert(az, key, (uint16_t)parsed_version);
    }
    free(response);
    return ok;
}

static bool az_delete(void *ctx, uint32_t key) {
    picowal_aztable_store_t *az = (picowal_aztable_store_t *)ctx;
    if (!az_ready(ctx)) return false;
    char entity[80];
    if (!az_entity(entity, sizeof(entity), key)) return false;
    bool ok = az_request(az, "DELETE", entity, NULL, NULL, NULL, 0);
    if (ok) index_delete(az, key);
    return ok;
}

static bool az_exists(void *ctx, uint32_t key) {
    picowal_aztable_store_t *az = (picowal_aztable_store_t *)ctx;
    return az_ready(ctx) && index_find(az, key) >= 0;
}

static uint32_t az_range(void *ctx, uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint32_t max) {
    picowal_aztable_store_t *az = (picowal_aztable_store_t *)ctx;
    if (!az_ready(ctx) || !out_keys || max == 0) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < az->index_count && count < max; i++) {
        uint32_t key = az->index[i].key;
        if ((key & mask) == (prefix & mask)) out_keys[count++] = key;
    }
    return count;
}

static const picowal_store_ops_t az_ops = {
    .name = "azure-table",
    .ready = az_ready,
    .put = az_put,
    .get_copy = az_get_copy,
    .del = az_delete,
    .exists = az_exists,
    .range = az_range,
};

bool picowal_store_aztable_open(picowal_aztable_store_t *ctx,
                                const picowal_aztable_config_t *config,
                                picowal_store_t *out_store) {
    if (!ctx || !config || !out_store ||
        !copy_text(ctx->endpoint, sizeof(ctx->endpoint), config->endpoint) ||
        !copy_text(ctx->sas, sizeof(ctx->sas), config->sas)) {
        return false;
    }
    ctx->index_count = 0;
    ctx->ready = true;
    out_store->ops = &az_ops;
    out_store->ctx = ctx;
    if (config->load_existing && !az_load_index(ctx)) {
        ctx->ready = false;
        return false;
    }
    return true;
}

