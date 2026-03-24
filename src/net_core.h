#ifndef NET_CORE_H
#define NET_CORE_H

#include "wal_defs.h"

// WiFi + server config
#define WIFI_SSID       "Puddles-Mesh"
#define WIFI_PASSWORD   "Whatever1"
#define WAL_HOST        "192.168.1.100"
#define WAL_PORT        8001
#define WIFI_TIMEOUT_MS 15000

// TCP wire protocol:
//
// Requests (host → pico):
//   [1 byte opcode][payload...]
//
//   APPEND:  0x01 [4 bytes key_hash LE] [2 bytes value_len LE] [1 byte delta_op] [value_len bytes]
//   READ:    0x02 [4 bytes key_hash LE]
//   NOOP:    0x00
//
// Responses (pico → host):
//   APPEND_ACK:  0x81 [4 bytes seq LE]
//   READ_RESP:   0x82 [4 bytes count LE] then for each delta:
//                     [4 bytes seq LE] [2 bytes len LE] [len bytes payload]
//   NOOP_ACK:    0x80
//   ERROR:       0xFF [1 byte error code]

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

// Core 0 entry: WiFi + TCP + WAL dispatch (never returns)
void net_core_run(wal_state_t *wal);

#endif
