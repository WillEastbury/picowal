#ifndef PICOWAL_STORE_FS_H
#define PICOWAL_STORE_FS_H

#include <stdbool.h>

#include "picowal_store.h"

#define PICOWAL_FS_ROOT_MAX 240

typedef struct picowal_fs_store {
    char root[PICOWAL_FS_ROOT_MAX];
    bool ready;
} picowal_fs_store_t;

bool picowal_store_fs_open(picowal_fs_store_t *ctx, const char *root, picowal_store_t *out_store);

#endif
