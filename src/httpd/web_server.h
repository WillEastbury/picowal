#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "wal_defs.h"

// Start the HTTP server on port 80.
void web_server_init(wal_state_t *wal);

// Set the PSK for HTTP auth (call before init).
void web_server_set_psk(const uint8_t psk[32]);

// True when HTTP traffic was seen recently enough that Core 0 should avoid
// optional UI work and prioritize network polling.
bool web_server_recent_activity(uint32_t quiet_ms);

#endif
