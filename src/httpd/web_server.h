#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "wal_defs.h"

// Start the HTTP server on port 80.
// Serves a minimal admin shell + REST API for KV operations.
// Must be called after WiFi is connected (from Core 0 context).
void web_server_init(wal_state_t *wal);

#endif
