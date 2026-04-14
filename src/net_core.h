#ifndef NET_CORE_H
#define NET_CORE_H

#include "wal_defs.h"

// WiFi config
#define WIFI_SSID       "Bussy5G"
#define WIFI_PASSWORD   "Whatever1"
#define WIFI_TIMEOUT_MS 15000

// TCP server config — Pico listens on this port
#define WAL_LISTEN_PORT 8001

// ============================================================
// Authentication: Pre-shared key challenge-response
//
// Handshake (after TCP connect):
//   1. Pico → Host:  CHALLENGE [16 bytes random nonce]
//   2. Host → Pico:  RESPONSE  [32 bytes HMAC(nonce, PSK)]
//   3. Pico verifies HMAC, sends AUTH_OK or AUTH_FAIL
//   4. Only after AUTH_OK are WAL ops accepted
//
// PSK is 32 bytes, configured below. Must match host-side.
// HMAC is a lightweight CRC32-chain keyed hash (same scheme
// as the WAL signature, not cryptographic — use TLS if you
// need real security on untrusted networks).
// ============================================================

#define AUTH_PSK { \
    0x50, 0x69, 0x63, 0x6F, 0x57, 0x41, 0x4C, 0x5F, \
    0x41, 0x75, 0x74, 0x68, 0x4B, 0x65, 0x79, 0x32, \
    0x30, 0x32, 0x36, 0x5F, 0x53, 0x65, 0x63, 0x72, \
    0x65, 0x74, 0x50, 0x53, 0x4B, 0x21, 0x21, 0x21  \
}

#define AUTH_NONCE_LEN    16
#define AUTH_RESPONSE_LEN 32

// Wire opcodes for auth handshake
#define WIRE_AUTH_CHALLENGE 0xA0  // Pico → Host: [16 nonce]
#define WIRE_AUTH_RESPONSE  0xA1  // Host → Pico: [32 hmac]
#define WIRE_AUTH_OK        0xA2  // Pico → Host: authenticated
#define WIRE_AUTH_FAIL      0xA3  // Pico → Host: rejected, disconnect

// WAL operation opcodes (only accepted after auth)
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
#define WIRE_ERR_AUTH   0x04

// Core 0 entry: WiFi + TCP server + auth + WAL dispatch (never returns)
void net_core_run(wal_state_t *wal);

#endif
