#ifndef NET_CORE_H
#define NET_CORE_H

#include "wal_defs.h"

// WiFi config
#define WIFI_SSID       "Bussy5G"
#define WIFI_PASSWORD   "Whatever1"
#define WIFI_TIMEOUT_MS 15000

// TCP server config — Pico listens on this port
#define WAL_LISTEN_PORT 8001

// WAL operation opcodes (TCP WAL protocol)
#define WIRE_OP_NOOP    0x00
#define WIRE_OP_APPEND  0x01
#define WIRE_OP_READ    0x02

#define WIRE_ACK_NOOP   0x80
#define WIRE_ACK_APPEND 0x81
#define WIRE_ACK_READ   0x82
#define WIRE_ERROR      0xFF

#define WIRE_ERR_FULL   0x01
#define WIRE_ERR_TOOBIG 0x02
#define WIRE_ERR_PROTO  0x03

// Core 0 entry: WiFi + TCP server + WAL dispatch (never returns)
void net_core_run(wal_state_t *wal);

#endif
