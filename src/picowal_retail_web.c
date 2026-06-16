#include "picowal_retail_web.h"

#if !defined(PICOWAL_HOST)
#error "picowal_retail_web is a host/server-side primitive."
#endif

#include <stdio.h>
#include <string.h>

static char response_buf[PICOWAL_RETAIL_JSON_MAX];

static const char retail_html[] =
#include "../host/www/retail_storefront.inc"
;

static int json_response(picoweb_response_t *response, int status) {
    response->status_code = status;
    response->content_type = "application/json";
    response->body = (const unsigned char *)response_buf;
    response->body_length = strlen(response_buf);
    return 1;
}

static int text_response(picoweb_response_t *response, int status, const char *text) {
    response->status_code = status;
    response->content_type = "text/plain";
    response->body = (const unsigned char *)text;
    response->body_length = strlen(text);
    return 1;
}

static void body_value(const picoweb_request_t *request, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!request || !request->body || request->body_length == 0 || !key) return;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *body = (const char *)request->body;
    const char *found = strstr(body, pattern);
    if (!found) return;
    found = strchr(found + strlen(pattern), ':');
    if (!found) return;
    found++;
    while (*found == ' ' || *found == '\t' || *found == '"') found++;
    size_t i = 0;
    while (i + 1u < cap && *found && *found != '"' && *found != ',' && *found != '}') {
        out[i++] = *found++;
    }
    out[i] = 0;
}

static int storefront(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    (void)request;
    (void)context;
    response->status_code = 200;
    response->content_type = "text/html";
    response->body = (const unsigned char *)retail_html;
    response->body_length = strlen(retail_html);
    return 1;
}

static int ingest_demo(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    (void)request;
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail) return text_response(response, 500, "retail not configured");
    if (picowal_retail_ingest_demo(web->retail) != PICOWAL_SEARCH_OK) {
        return text_response(response, 500, "ingest failed");
    }
    snprintf(response_buf, sizeof(response_buf), "{\"ingested\":true}");
    return json_response(response, 200);
}

static int products(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    (void)request;
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail) return text_response(response, 500, "retail not configured");
    if (picowal_retail_products_json(web->retail, response_buf, sizeof(response_buf)) != PICOWAL_SEARCH_OK) {
        return text_response(response, 500, "products failed");
    }
    return json_response(response, 200);
}

static int product_detail(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail || request->route_param_count < 1) return text_response(response, 500, "retail not configured");
    picowal_search_status_t st = picowal_retail_product_json(web->retail, request->route_params[0],
                                                             response_buf, sizeof(response_buf));
    if (st == PICOWAL_SEARCH_NOT_FOUND) return text_response(response, 404, "not found");
    if (st != PICOWAL_SEARCH_OK) return text_response(response, 500, "product failed");
    return json_response(response, 200);
}

static int search(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail) return text_response(response, 500, "retail not configured");
    char query[128];
    body_value(request, "query", query, sizeof(query));
    if (picowal_retail_search_json(web->retail, query, response_buf, sizeof(response_buf)) != PICOWAL_SEARCH_OK) {
        return text_response(response, 500, "search failed");
    }
    return json_response(response, 200);
}

static int recommend(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail) return text_response(response, 500, "retail not configured");
    char id[PICOWAL_RETAIL_ID_MAX + 1u];
    body_value(request, "id", id, sizeof(id));
    picowal_search_status_t st = picowal_retail_recommend_json(web->retail, id, response_buf, sizeof(response_buf));
    if (st == PICOWAL_SEARCH_NOT_FOUND) return text_response(response, 404, "not found");
    if (st != PICOWAL_SEARCH_OK) return text_response(response, 500, "recommend failed");
    return json_response(response, 200);
}

static int event_write(const picoweb_request_t *request, picoweb_response_t *response, void *context) {
    picowal_retail_web_t *web = (picowal_retail_web_t *)context;
    if (!web || !web->retail) return text_response(response, 500, "retail not configured");
    char visitor[32], event[32], product[PICOWAL_RETAIL_ID_MAX + 1u];
    body_value(request, "visitorId", visitor, sizeof(visitor));
    body_value(request, "eventType", event, sizeof(event));
    body_value(request, "productId", product, sizeof(product));
    if (picowal_retail_record_event(web->retail, web->event_pack, visitor, event, product) != PICOWAL_SEARCH_OK) {
        return text_response(response, 500, "event failed");
    }
    snprintf(response_buf, sizeof(response_buf), "{\"accepted\":true}");
    return json_response(response, 200);
}

static const picoweb_route_t retail_routes[] = {
    { PICOWEB_METHOD_GET, "/", storefront },
    { PICOWEB_METHOD_GET, "/retail", storefront },
    { PICOWEB_METHOD_POST, "/api/retail/products:ingestDemo", ingest_demo },
    { PICOWEB_METHOD_GET, "/api/retail/products", products },
    { PICOWEB_METHOD_GET, "/api/retail/products/{id}", product_detail },
    { PICOWEB_METHOD_POST, "/api/retail/search", search },
    { PICOWEB_METHOD_POST, "/api/retail/recommend", recommend },
    { PICOWEB_METHOD_POST, "/api/retail/events", event_write },
};

const picoweb_route_table_t picowal_retail_route_table = {
    "picowal-retail",
    retail_routes,
    sizeof(retail_routes) / sizeof(retail_routes[0]),
};
