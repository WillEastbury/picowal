#ifndef PICOWAL_RETAIL_WEB_H
#define PICOWAL_RETAIL_WEB_H

#include "picowal_retail.h"
#include "picoweb.h"

typedef struct {
    picowal_retail_t *retail;
    uint16_t event_pack;
} picowal_retail_web_t;

extern const picoweb_route_table_t picowal_retail_route_table;

#endif
