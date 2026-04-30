#ifndef PICOCLUSTER_RING_H
#define PICOCLUSTER_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// ============================================================
// Ring interconnect driver — PIO + DMA based
// ============================================================

// Ring configuration per link
typedef struct {
    uint8_t  pio_block;    // 0, 1, or 2
    uint8_t  sm_tx;        // TX state machine index
    uint8_t  sm_rx;        // RX state machine index
    uint8_t  pin_tx;       // GPIO for TX
    uint8_t  pin_rx;       // GPIO for RX
    uint8_t  dma_ch_tx;    // DMA channel for TX
    uint8_t  dma_ch_rx;    // DMA channel for RX
    uint32_t baud_rate;    // Target baud (bit rate / 2 for manchester)
} ring_config_t;

// Ring state
typedef struct {
    ring_config_t config;
    // RX buffer (double-buffered)
    uint8_t  rx_buf[2][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint8_t  rx_active_buf;   // Which buffer DMA is writing to
    uint32_t rx_len;          // Bytes received in current packet
    bool     rx_ready;        // Packet ready for processing
    // TX buffer
    uint8_t  tx_buf[PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint32_t tx_len;
    bool     tx_busy;
    // Stats
    uint32_t packets_rx;
    uint32_t packets_tx;
    uint32_t packets_forwarded;
    uint32_t crc_errors;
} ring_state_t;

// Global ring array
extern ring_state_t rings[RING_COUNT];

// Initialise all ring hardware (PIO programs, DMA, GPIOs)
void ring_init_all(const ring_config_t configs[RING_COUNT]);

// Initialise a single ring
void ring_init(uint8_t ring_id, const ring_config_t *config);

// Poll for received packets (call from main loop or ISR)
// Returns true if a packet is ready, fills header and payload pointer
bool ring_poll_rx(uint8_t ring_id, pkt_header_t *hdr, uint8_t **payload);

// Mark RX buffer as consumed (re-arm DMA)
void ring_rx_done(uint8_t ring_id);

// Send a packet on a ring (non-blocking, queues to DMA)
bool ring_send(uint8_t ring_id, const pkt_header_t *hdr,
               const uint8_t *payload, uint32_t payload_len);

// Forward a raw packet to another ring (zero-copy if possible)
void ring_forward(uint8_t src_ring, uint8_t dst_ring);

// Check if TX is idle
bool ring_tx_ready(uint8_t ring_id);

// --- Snooping / caching hook ---
// Called by RX ISR for every packet that passes through
typedef void (*ring_snoop_cb)(uint8_t ring_id, const pkt_header_t *hdr,
                              const uint8_t *payload);
void ring_set_snoop_callback(ring_snoop_cb cb);

// --- Relay logic ---
// Process incoming packet: consume if for us, forward otherwise
// Returns true if packet was consumed locally
bool ring_process_packet(uint8_t ring_id, uint8_t my_node_id);

#endif // PICOCLUSTER_RING_H
