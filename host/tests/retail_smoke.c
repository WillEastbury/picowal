#define _GNU_SOURCE

#include "picowal_api.h"
#include "picowal_retail.h"
#include "picowal_retail_web.h"
#include "picowal_store_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static int call_route(size_t route_index,
                      picowal_retail_web_t *web,
                      const unsigned char *body,
                      size_t body_len,
                      picoweb_response_t *response) {
    picoweb_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = picowal_retail_route_table.routes[route_index].method;
    request.path = picowal_retail_route_table.routes[route_index].pattern;
    request.body = body;
    request.body_length = body_len;
    return picowal_retail_route_table.routes[route_index].handler(&request, response, web);
}

int main(void) {
    char root[] = "/tmp/picowal-retail-smoke-XXXXXX";
    if (!mkdtemp(root)) return 1;

    picowal_fs_store_t fs;
    picowal_store_t store;
    if (!picowal_store_fs_open(&fs, root, &store)) return 1;
    picowal_api_set_store(&store);

    static picowal_retail_t retail;
    picowal_retail_init(&retail, 8, 9, 100, 10, 200);
    if (picowal_retail_ingest_demo(&retail) != PICOWAL_SEARCH_OK) return 1;

    char json[PICOWAL_RETAIL_JSON_MAX];
    if (picowal_retail_products_json(&retail, json, sizeof(json)) != PICOWAL_SEARCH_OK) return 1;
    if (!contains(json, "Aurora Storm Shell Jacket")) return 1;

    if (picowal_retail_search_json(&retail, "waterproof jacket", json, sizeof(json)) != PICOWAL_SEARCH_OK) return 1;
    if (!contains(json, "results") || !contains(json, "outerwear")) return 1;

    if (picowal_retail_product_json(&retail, "aurora-shell", json, sizeof(json)) != PICOWAL_SEARCH_OK) return 1;
    if (!contains(json, "Contoso Trail")) return 1;

    if (picowal_retail_recommend_json(&retail, "aurora-shell", json, sizeof(json)) != PICOWAL_SEARCH_OK) return 1;
    if (!contains(json, "results")) return 1;

    if (picowal_retail_record_event(&retail, 11, "visitor-1", "detail-page-view", "aurora-shell") != PICOWAL_SEARCH_OK) return 1;

    picowal_retail_web_t web = {
        .retail = &retail,
        .event_pack = 11,
    };
    picoweb_response_t response;
    if (!call_route(0, &web, NULL, 0, &response) || response.status_code != 200 ||
        !contains((const char *)response.body, "BareMetal.Communications")) return 1;
    if (!call_route(2, &web, (const unsigned char *)"{}", 2, &response) || response.status_code != 200) return 1;
    const unsigned char search_body[] = "{\"query\":\"waterproof jacket\"}";
    if (!call_route(5, &web, search_body, sizeof(search_body) - 1u, &response) ||
        response.status_code != 200 || !contains((const char *)response.body, "results")) return 1;

    puts("picowal retail smoke ok");
    rmdir(root);
    return 0;
}
