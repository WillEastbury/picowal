#ifndef PICOCLUSTER_PACKET_H
#define PICOCLUSTER_PACKET_H

#include <stdint.h>

// ============================================================
// Ring packet protocol
// ============================================================

// Packet types
#define PKT_EXEC       0x01  // Execute card with data
#define PKT_RESULT     0x02  // Execution result
#define PKT_CARD_DATA  0x03  // Card bytecode (for caching)
#define PKT_NAK        0x04  // Card cache miss
#define PKT_STATUS     0x05  // Heartbeat/status
#define PKT_BATCH      0x06  // Batch execution (multiple items)
#define PKT_BATCH_RES  0x07  // Batch result
#define PKT_DISCOVER   0x08  // Topology discovery
#define PKT_ROUTE      0x09  // Routing table update
#define PKT_CARD_REQ   0x0A  // Request a card by ID

// Special addresses
#define ADDR_BROADCAST 0xFF
#define ADDR_MASTER    0x00

// Maximum payload
#define PKT_MAX_PAYLOAD  4096
#define PKT_HEADER_SIZE  8

// Ring IDs
#define RING_EXPRESS_1   0
#define RING_EXPRESS_2   1
#define RING_NORMAL      2
#define RING_STORAGE     3
#define RING_COUNT       4

// Packet header (8 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  dest;         // Destination node ID
    uint8_t  src;          // Source node ID
    uint8_t  type;         // Packet type (PKT_*)
    uint8_t  flags;        // Flags (TTL in bits 0-3, priority in 4-5)
    uint16_t payload_len;  // Payload length in bytes
    uint16_t crc16;        // CRC16 over header + payload
} pkt_header_t;

// EXEC payload
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t data_len;
    uint8_t  data[];       // Input bytearray
} pkt_exec_t;

// BATCH payload
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t item_count;
    uint16_t item_size;    // Fixed size per item (0 = variable)
    uint8_t  data[];       // Batch items
} pkt_batch_t;

// RESULT payload
typedef struct __attribute__((packed)) {
    uint8_t  status;       // 0=OK, 1=ERROR, 2=TIMEOUT
    uint8_t  card_major;
    uint8_t  card_minor;
    uint8_t  reserved;
    uint16_t data_len;
    uint8_t  data[];       // Result bytearray
} pkt_result_t;

// CARD_DATA payload
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t version;
    uint32_t bytecode_len; // In bytes (must be multiple of 4)
    uint8_t  bytecode[];   // Card instructions
} pkt_card_data_t;

// NAK payload
typedef struct __attribute__((packed)) {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t version;      // Version requested (0 = any)
} pkt_nak_t;

// STATUS payload
typedef struct __attribute__((packed)) {
    uint8_t  node_state;   // 0=idle, 1=busy, 2=error
    uint8_t  queue_depth;  // Pending exec requests
    uint16_t card_count;   // Cards in local cache
    uint32_t exec_count;   // Total cards executed since boot
    uint32_t uptime_sec;   // Seconds since boot
} pkt_status_t;

// --- Flags field encoding ---
#define PKT_TTL(flags)        ((flags) & 0x0F)
#define PKT_PRIORITY(flags)   (((flags) >> 4) & 0x03)
#define PKT_MAKE_FLAGS(ttl, pri) (((pri) << 4) | ((ttl) & 0x0F))

// Default TTL (max hops before drop)
#define DEFAULT_TTL  15

// --- CRC16 (CCITT) ---
uint16_t crc16_ccitt(const uint8_t *data, uint32_t len);

#endif // PICOCLUSTER_PACKET_H
