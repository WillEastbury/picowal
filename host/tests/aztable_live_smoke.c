#include "picowal_api.h"
#include "picowal_store_aztable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *endpoint = getenv("PICOWAL_AZTABLE_ENDPOINT");
    const char *sas = getenv("PICOWAL_AZTABLE_SAS");
    if (!endpoint || !sas || !*endpoint || !*sas) {
        puts("picowal azure table live smoke skipped");
        return 0;
    }

    picowal_aztable_store_t az;
    picowal_store_t store;
    picowal_aztable_config_t cfg = {
        .endpoint = endpoint,
        .sas = sas,
        .load_existing = true,
    };
    if (!picowal_store_aztable_open(&az, &cfg, &store)) return 1;
    picowal_api_set_store(&store);

    const uint16_t pack = 2;
    const uint32_t card = 0x12345u;
    const char value[] = "hello-azure-table";
    char out[64];
    uint16_t out_len = sizeof(out);

    if (picowal_api_put_create(pack, card, value, sizeof(value)) != PICOWAL_API_OK) return 1;
    if (picowal_api_get(pack, card, out, sizeof(out), &out_len) != PICOWAL_API_OK) return 1;
    if (out_len != sizeof(value) || memcmp(out, value, sizeof(value)) != 0) return 1;
    if (!picowal_api_exists(pack, card)) return 1;

    uint32_t cards[16];
    uint32_t count = picowal_api_list(pack, cards, 16);
    bool listed = false;
    for (uint32_t i = 0; i < count; i++) {
        if (cards[i] == card) listed = true;
    }
    if (!listed) return 1;
    if (picowal_api_delete(pack, card) != PICOWAL_API_OK) return 1;
    if (picowal_api_exists(pack, card)) return 1;

    puts("picowal azure table live smoke ok");
    return 0;
}

