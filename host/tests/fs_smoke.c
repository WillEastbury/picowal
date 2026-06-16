#define _GNU_SOURCE

#include "picowal_api.h"
#include "picowal_store_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char root[] = "/tmp/picowal-fs-smoke-XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }

    picowal_fs_store_t fs;
    picowal_store_t store;
    if (!picowal_store_fs_open(&fs, root, &store)) {
        fprintf(stderr, "failed to open fs store\n");
        return 1;
    }
    picowal_api_set_store(&store);

    const char value[] = "hello-picowal";
    if (picowal_api_put_create(2, 1, value, sizeof(value)) != PICOWAL_API_OK) return 1;
    if (picowal_api_put_create(2, 1, value, sizeof(value)) != PICOWAL_API_EXISTS) return 1;

    char out[64];
    uint16_t out_len = 0;
    if (picowal_api_get(2, 1, out, sizeof(out), &out_len) != PICOWAL_API_OK) return 1;
    if (out_len != sizeof(value) || memcmp(out, value, sizeof(value)) != 0) return 1;

    uint32_t cards[4];
    uint32_t count = picowal_api_list(2, cards, 4);
    if (count != 1 || cards[0] != 1) return 1;

    const char updated[] = "updated-picowal";
    if (picowal_api_put(2, 1, updated, sizeof(updated)) != PICOWAL_API_OK) return 1;
    memset(out, 0, sizeof(out));
    out_len = 0;
    if (picowal_api_get(2, 1, out, sizeof(out), &out_len) != PICOWAL_API_OK) return 1;
    if (out_len != sizeof(updated) || memcmp(out, updated, sizeof(updated)) != 0) return 1;

    count = picowal_api_list(2, cards, 4);
    if (count != 1 || cards[0] != 1) return 1;

    if (!picowal_api_exists(2, 1)) return 1;
    if (picowal_api_delete(2, 1) != PICOWAL_API_OK) return 1;
    if (picowal_api_exists(2, 1)) return 1;
    if (picowal_api_get(2, 1, out, sizeof(out), &out_len) != PICOWAL_API_NOT_FOUND) return 1;

    printf("picowal fs smoke ok: %s\n", root);
    char pack_dir[128];
    snprintf(pack_dir, sizeof(pack_dir), "%s/002", root);
    rmdir(pack_dir);
    rmdir(root);
    return 0;
}
