#ifndef PICOWAL_STORE_AZTABLE_H
#define PICOWAL_STORE_AZTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "picowal_store.h"

#define PICOWAL_AZTABLE_ENDPOINT_MAX 256
#define PICOWAL_AZTABLE_SAS_MAX      512
#define PICOWAL_AZTABLE_INDEX_MAX    8192

typedef struct {
    const char *endpoint;      // e.g. https://acct.table.core.windows.net/Table
    const char *sas;           // leading '?' optional
    bool load_existing;        // load PartitionKey/RowKey/Version into memory on open
} picowal_aztable_config_t;

typedef struct {
    uint32_t key;
    uint16_t version;
} picowal_aztable_index_entry_t;

typedef struct {
    char endpoint[PICOWAL_AZTABLE_ENDPOINT_MAX];
    char sas[PICOWAL_AZTABLE_SAS_MAX];
    picowal_aztable_index_entry_t index[PICOWAL_AZTABLE_INDEX_MAX];
    uint32_t index_count;
    bool ready;
} picowal_aztable_store_t;

bool picowal_store_aztable_open(picowal_aztable_store_t *ctx,
                                const picowal_aztable_config_t *config,
                                picowal_store_t *out_store);

#endif
