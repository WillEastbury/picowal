#include "picowal_api.h"
#include "picowal_store_aztable.h"

#include <stdio.h>

int main(void) {
    picowal_aztable_store_t az;
    picowal_store_t store;
    picowal_aztable_config_t bad = {
        .endpoint = "",
        .sas = "",
        .load_existing = false,
    };
    if (picowal_store_aztable_open(&az, &bad, &store)) return 1;

    picowal_aztable_config_t cfg = {
        .endpoint = "https://example.table.core.windows.net/Picowal",
        .sas = "?sv=fake",
        .load_existing = false,
    };
    if (!picowal_store_aztable_open(&az, &cfg, &store)) return 1;
    picowal_api_set_store(&store);

    uint32_t cards[4];
    if (picowal_api_list(2, cards, 4) != 0) return 1;
    if (picowal_api_put_create(1, 1, "x", 1) != PICOWAL_API_INVALID) return 1;

    printf("picowal azure table config smoke ok\n");
    return 0;
}

