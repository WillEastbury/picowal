#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "wal_defs.h"

// Start the HTTP server on port 80.
void web_server_init(wal_state_t *wal);

// True when HTTP traffic was seen recently enough that Core 0 should avoid
// optional UI work and prioritize network polling.
bool web_server_recent_activity(uint32_t quiet_ms);

// Debug log — writes to ring buffer accessible via GET /admin/log
void web_log(const char *fmt, ...);

// Cardinality bucket for a pack (0=<10, 1=10-99, 2=100-999, ...)
// Cached in SRAM, lazy-populated on first access
uint8_t get_cardinality(uint16_t pack);
void set_cardinality(uint16_t pack, uint32_t count);
bool flush_cardinality_one(void);  // flush one dirty entry to flash, call from idle

#endif
