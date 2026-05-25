#define _GNU_SOURCE

#include "picowal_store_fs.h"

#if defined(_WIN32)
#error "picowal_store_fs is a POSIX/Linux backend; do not compile it into Windows or Pico firmware targets."
#endif

#include "picowal_api.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PICOWAL_FS_PATH_MAX 512
#define PICOWAL_FS_MAGIC    0x504B564Cu /* "PKVL" */
#define PICOWAL_FS_VERSION  1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;
    uint16_t flags;
    uint32_t key;
    uint16_t value_len;
    uint16_t key_version;
    uint32_t crc32;
} picowal_fs_header_t;

static uint32_t fs_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool fs_mkdir_if_missing(const char *path) {
    if (mkdir(path, 0700) == 0) return true;
    if (errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return false;
}

static bool fs_pack_dir(const picowal_fs_store_t *ctx, uint16_t pack,
                        char *path, size_t path_len) {
    int n = snprintf(path, path_len, "%s/%03x", ctx->root, pack & PICOWAL_PACK_MAX);
    return n > 0 && (size_t)n < path_len;
}

static bool fs_card_path(const picowal_fs_store_t *ctx, uint32_t key,
                         char *path, size_t path_len) {
    uint16_t pack = picowal_api_key_pack(key);
    uint32_t card = picowal_api_key_card(key);
    int n = snprintf(path, path_len, "%s/%03x/%06lx.kv",
                     ctx->root, pack & PICOWAL_PACK_MAX, (unsigned long)(card & PICOWAL_CARD_MAX));
    return n > 0 && (size_t)n < path_len;
}

static bool fs_temp_path(const char *path, char *tmp, size_t tmp_len) {
    int n = snprintf(tmp, tmp_len, "%s.tmp.%ld", path, (long)getpid());
    return n > 0 && (size_t)n < tmp_len;
}

static bool fs_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool fs_read_header(const char *path, picowal_fs_header_t *hdr) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint8_t *p = (uint8_t *)hdr;
    size_t remaining = sizeof(*hdr);
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) {
            close(fd);
            return false;
        }
        p += n;
        remaining -= (size_t)n;
    }
    close(fd);
    return hdr->magic == PICOWAL_FS_MAGIC &&
           hdr->format_version == PICOWAL_FS_VERSION &&
           hdr->value_len <= PICOWAL_VALUE_MAX;
}

static bool fs_ready(void *ctx) {
    return ctx && ((picowal_fs_store_t *)ctx)->ready;
}

static bool fs_put(void *ctx, uint32_t key, const uint8_t *value, uint16_t len) {
    picowal_fs_store_t *fs = (picowal_fs_store_t *)ctx;
    if (!fs_ready(ctx) || len > PICOWAL_VALUE_MAX || (!value && len != 0)) return false;

    char dir[PICOWAL_FS_PATH_MAX];
    char path[PICOWAL_FS_PATH_MAX];
    char tmp[PICOWAL_FS_PATH_MAX];
    if (!fs_pack_dir(fs, picowal_api_key_pack(key), dir, sizeof(dir))) return false;
    if (!fs_mkdir_if_missing(dir)) return false;
    if (!fs_card_path(fs, key, path, sizeof(path))) return false;
    if (!fs_temp_path(path, tmp, sizeof(tmp))) return false;

    picowal_fs_header_t old_hdr;
    uint16_t version = fs_read_header(path, &old_hdr) ? (uint16_t)(old_hdr.key_version + 1u) : 1u;
    picowal_fs_header_t hdr = {
        .magic = PICOWAL_FS_MAGIC,
        .format_version = PICOWAL_FS_VERSION,
        .flags = 0,
        .key = key,
        .value_len = len,
        .key_version = version,
        .crc32 = fs_crc32(value, len),
    };

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = fs_write_all(fd, &hdr, sizeof(hdr)) && fs_write_all(fd, value, len) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

static bool fs_get_copy(void *ctx, uint32_t key, uint8_t *out, uint16_t *len, uint16_t *version) {
    picowal_fs_store_t *fs = (picowal_fs_store_t *)ctx;
    if (!fs_ready(ctx) || !out || !len) return false;

    char path[PICOWAL_FS_PATH_MAX];
    if (!fs_card_path(fs, key, path, sizeof(path))) return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    picowal_fs_header_t hdr;
    bool ok = false;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) goto done;
    if (hdr.magic != PICOWAL_FS_MAGIC || hdr.format_version != PICOWAL_FS_VERSION ||
        hdr.key != key || hdr.value_len > PICOWAL_VALUE_MAX || hdr.value_len > *len) {
        goto done;
    }
    uint16_t value_len = hdr.value_len;
    uint8_t buf[PICOWAL_VALUE_MAX];
    uint8_t *p = buf;
    size_t remaining = value_len;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            goto done;
        }
        if (n == 0) goto done;
        p += n;
        remaining -= (size_t)n;
    }
    if (fs_crc32(buf, value_len) != hdr.crc32) goto done;
    memcpy(out, buf, value_len);
    *len = value_len;
    if (version) *version = hdr.key_version;
    ok = true;
done:
    close(fd);
    return ok;
}

static bool fs_delete(void *ctx, uint32_t key) {
    picowal_fs_store_t *fs = (picowal_fs_store_t *)ctx;
    char path[PICOWAL_FS_PATH_MAX];
    if (!fs_ready(ctx) || !fs_card_path(fs, key, path, sizeof(path))) return false;
    return unlink(path) == 0;
}

static bool fs_exists(void *ctx, uint32_t key) {
    picowal_fs_store_t *fs = (picowal_fs_store_t *)ctx;
    char path[PICOWAL_FS_PATH_MAX];
    struct stat st;
    return fs_ready(ctx) && fs_card_path(fs, key, path, sizeof(path)) &&
           stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool fs_parse_hex_name(const char *name, uint32_t max, uint32_t *value) {
    uint32_t v = 0;
    uint32_t digits = 0;
    const char *dot = NULL;
    for (; *name; name++) {
        if (*name == '.') {
            dot = name;
            break;
        }
        uint8_t d;
        if (*name >= '0' && *name <= '9') d = (uint8_t)(*name - '0');
        else if (*name >= 'a' && *name <= 'f') d = (uint8_t)(*name - 'a' + 10);
        else if (*name >= 'A' && *name <= 'F') d = (uint8_t)(*name - 'A' + 10);
        else return false;
        v = (v << 4) | d;
        digits++;
    }
    if (dot && strcmp(dot, ".kv") != 0) return false;
    if (digits == 0 || v > max) return false;
    *value = v;
    return true;
}

static uint32_t fs_scan_pack(picowal_fs_store_t *fs, uint16_t pack, uint32_t prefix,
                             uint32_t mask, uint32_t *out_keys, uint32_t max) {
    char dir_path[PICOWAL_FS_PATH_MAX];
    if (!fs_pack_dir(fs, pack, dir_path, sizeof(dir_path))) return 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    uint32_t count = 0;
    struct dirent *ent;
    while (count < max && (ent = readdir(dir)) != NULL) {
        uint32_t card;
        if (!fs_parse_hex_name(ent->d_name, PICOWAL_CARD_MAX, &card)) continue;
        uint32_t key = picowal_api_key(pack, card);
        if ((key & mask) == (prefix & mask)) {
            out_keys[count++] = key;
        }
    }
    closedir(dir);
    return count;
}

static uint32_t fs_range(void *ctx, uint32_t prefix, uint32_t mask, uint32_t *out_keys, uint32_t max) {
    picowal_fs_store_t *fs = (picowal_fs_store_t *)ctx;
    if (!fs_ready(ctx) || !out_keys || max == 0) return 0;

    uint16_t prefix_pack = picowal_api_key_pack(prefix);
    if ((mask & 0xFFC00000u) == 0xFFC00000u) {
        return fs_scan_pack(fs, prefix_pack, prefix, mask, out_keys, max);
    }

    DIR *root = opendir(fs->root);
    if (!root) return 0;
    uint32_t count = 0;
    struct dirent *ent;
    while (count < max && (ent = readdir(root)) != NULL) {
        uint32_t pack;
        if (!fs_parse_hex_name(ent->d_name, PICOWAL_PACK_MAX, &pack)) continue;
        count += fs_scan_pack(fs, (uint16_t)pack, prefix, mask, out_keys + count, max - count);
    }
    closedir(root);
    return count;
}

static const picowal_store_ops_t fs_ops = {
    .name = "fs",
    .ready = fs_ready,
    .put = fs_put,
    .get_copy = fs_get_copy,
    .del = fs_delete,
    .exists = fs_exists,
    .range = fs_range,
};

bool picowal_store_fs_open(picowal_fs_store_t *ctx, const char *root, picowal_store_t *out_store) {
    if (!ctx || !root || !out_store) return false;
    size_t len = strlen(root);
    if (len == 0 || len >= sizeof(ctx->root)) return false;
    memcpy(ctx->root, root, len + 1);
    ctx->ready = fs_mkdir_if_missing(ctx->root);
    out_store->ops = &fs_ops;
    out_store->ctx = ctx;
    return ctx->ready;
}

