#ifndef UDP_WAL_H
#define UDP_WAL_H

#include "wal_defs.h"

// Pre-shared key for UDP WAL session key derivation (HKDF-SHA256).
// Must match the client-side PSK. Used only for ChaCha20-Poly1305 AEAD.
#define UDP_WAL_PSK { \
    0x50, 0x69, 0x63, 0x6F, 0x57, 0x41, 0x4C, 0x5F, \
    0x41, 0x75, 0x74, 0x68, 0x4B, 0x65, 0x79, 0x32, \
    0x30, 0x32, 0x36, 0x5F, 0x53, 0x65, 0x63, 0x72, \
    0x65, 0x74, 0x50, 0x53, 0x4B, 0x21, 0x21, 0x21  \
}

// ============================================================
// UDP WAL Protocol — port 8002
//
// Unencrypted phase 1. Session IDs for correlation only.
// Encryption (ChaCha20-Poly1305) to be added in phase 2.
//
// Wire format (per datagram, max 1400 bytes):
//   [session_id:8][epoch:2][seq:4][msg_type:1][payload...]
//
// Msg types:
//   0x10 HELLO         client→server  [client_random:16]
//   0x11 HELLO_OK      server→client  [session_id:8][server_random:16][epoch:4]
//   0x12 RESUME        client→server  [session_id:8]
//   0x13 RESUME_OK     server→client  [epoch:4]
//   0x14 RESUME_FAIL   server→client  (empty)
//
//   0x20 BATCH_WRITE   client→server  [batch_seq:2][count:1][durability:1][cards...]
//                      each card: [pack:2][card:4][len:2][payload:len]
//   0x21 BATCH_QUEUED  server→client  [batch_seq:2][count:1][bitmap:4][est_ms:2]
//   0x22 BATCH_COMMITTED server→client [batch_seq:2][count:1][bitmap:4]
//
//   0x30 READ          client→server  [pack:2][card:4]
//   0x31 DATA          server→client  [pack:2][card:4][len:2][payload:len]
//   0x32 NOT_FOUND     server→client  [pack:2][card:4]
//
//   0x40 BACKPRESSURE  server→client  [max_inflight:1]
//
// Durability flags:
//   0x01 FIRE_AND_FORGET  — no response
//   0x02 ACK_QUEUED       — bitmap when in SRAM
//   0x03 ACK_DURABLE      — queued + committed bitmaps
//   0x04 ACK_ALL_COMMITTED — single response after full batch on SD
// ============================================================

#define UDP_WAL_PORT        8002
#define UDP_WAL_MAX_DGRAM   1400
#define UDP_WAL_HDR_SIZE    15      // session_id(8) + epoch(2) + seq(4) + msg_type(1)
#define UDP_WAL_MAX_PAYLOAD (UDP_WAL_MAX_DGRAM - UDP_WAL_HDR_SIZE)

#define UDP_WAL_MAX_SESSIONS 8
#define UDP_WAL_MAX_BATCH    16

// Message types
#define UMSG_HELLO          0x10
#define UMSG_HELLO_OK       0x11
#define UMSG_RESUME         0x12
#define UMSG_RESUME_OK      0x13
#define UMSG_RESUME_FAIL    0x14

#define UMSG_BATCH_WRITE    0x20
#define UMSG_BATCH_QUEUED   0x21
#define UMSG_BATCH_COMMITTED 0x22

#define UMSG_READ           0x30
#define UMSG_DATA           0x31
#define UMSG_NOT_FOUND      0x32

#define UMSG_BACKPRESSURE   0x40

// Durability levels
#define UDUR_FIRE_AND_FORGET  0x01
#define UDUR_ACK_QUEUED       0x02
#define UDUR_ACK_DURABLE      0x03
#define UDUR_ACK_ALL_COMMITTED 0x04

// Initialize UDP WAL listener on port 8002
void udp_wal_init(wal_state_t *wal);

// Poll for pending committed batches (call from main loop)
void udp_wal_poll(void);

#endif
