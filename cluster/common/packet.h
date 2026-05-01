#ifndef PICOCLUSTER_PACKET_H
#define PICOCLUSTER_PACKET_H

#include <stdint.h>

// ============================================================
// Packet protocol — point-to-point, no addressing needed
// ============================================================
// Every wire is a dedicated link. The physical connection IS the
// address. Just send a command and wait for a reply.
//
// Frame: [type:1][len:2][payload:N][crc16:2]
// Total overhead: 5 bytes. That's it.

// --- Packet types ---
#define PKT_EXEC       0x01  // Execute card (head → worker)
#define PKT_RESULT     0x02  // Execution result (worker → head)
#define PKT_CARD_REQ   0x03  // Request card bytecode (worker → storage)
#define PKT_CARD_DATA  0x04  // Card bytecode (storage → worker)
#define PKT_STATUS     0x05  // Heartbeat/status (worker → head, periodic)
#define PKT_BATCH      0x06  // Batch execution (head → worker)
#define PKT_BATCH_RES  0x07  // Batch result (worker → head)
#define PKT_NAK        0x08  // Card not found (storage → worker)
#define PKT_PING       0x09  // Health check (any direction)
#define PKT_PONG       0x0A  // Health response

// --- Packet header (5 bytes, minimal) ---
typedef struct __attribute__((packed)) {
    uint8_t  type;         // PKT_* type
    uint16_t payload_len;  // Payload length in bytes
    uint16_t crc16;        // CRC16 over payload
} pkt_header_t;

#define PKT_HEADER_SIZE  5
#define PKT_MAX_PAYLOAD  4096

// --- EXEC payload ---
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t data_len;
    uint8_t  data[];       // Input data
} pkt_exec_t;

// --- RESULT payload ---
typedef struct __attribute__((packed)) {
    uint8_t  status;       // 0=OK, 1=ERROR, 2=CARD_MISSING
    uint8_t  card_major;
    uint8_t  card_minor;
    uint8_t  reserved;
    uint16_t data_len;
    uint8_t  data[];       // Result data
} pkt_result_t;

// --- CARD_REQ payload ---
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
} pkt_card_req_t;

// --- CARD_DATA payload ---
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t version;
    uint32_t bytecode_len;
    uint8_t  bytecode[];
} pkt_card_data_t;

// --- BATCH payload ---
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t item_count;
    uint16_t item_size;    // 0 = variable
    uint8_t  data[];
} pkt_batch_t;

// --- STATUS payload ---
typedef struct __attribute__((packed)) {
    uint8_t  busy;         // 0=idle, 1=executing
    uint8_t  queue_depth;
    uint16_t card_count;   // Cards in cache
    uint32_t exec_count;   // Total executed
} pkt_status_t;

// --- CRC16 ---
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len);

#endif // PICOCLUSTER_PACKET_H
