#ifndef PICOCLUSTER_LINK_H
#define PICOCLUSTER_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// ============================================================
// Half-duplex single-wire PIO link driver
// ============================================================
// All links use Manchester encoding on 1 GPIO, with PIO switching
// direction. Protocol is master-initiated:
//   Master sends → turnaround gap → slave replies → turnaround
//
// Used by:
//   Head (master on command links to workers)
//   Storage (master on data links to workers, also cross-connects)
//   Worker (slave on both head and storage links)

// --- Half-duplex link state ---
typedef struct {
    uint8_t  pin;         // GPIO pin
    uint8_t  sm;          // PIO state machine
    uint8_t  pio_idx;     // PIO block (0, 1, 2)
    uint8_t  dma_ch;      // DMA channel
    bool     is_master;   // Master controls direction
} link_port_t;

// --- Init a half-duplex port ---
void link_init_port(link_port_t *port);

// --- Master API (head/storage → worker) ---
// Send packet then switch to receive mode for reply
bool link_master_send(link_port_t *port, const pkt_header_t *hdr,
                      const uint8_t *payload, uint16_t len);

// Poll for reply after sending (non-blocking)
bool link_master_poll_reply(link_port_t *port, pkt_header_t *hdr, uint8_t **payload);

// Send and wait for reply (blocking with timeout)
bool link_master_transact(link_port_t *port, const pkt_header_t *hdr,
                          const uint8_t *payload, uint16_t len,
                          pkt_header_t *reply_hdr, uint8_t **reply_payload,
                          uint32_t timeout_us);

// --- Slave API (worker responding to head/storage) ---
// Poll for incoming request from master
bool link_slave_poll_rx(link_port_t *port, pkt_header_t *hdr, uint8_t **payload);

// Send reply back to master (switches to TX briefly, then back to RX)
void link_slave_reply(link_port_t *port, const pkt_header_t *hdr,
                      const uint8_t *payload, uint16_t len);

// --- Multi-port helpers (head/storage) ---

// Init N ports starting at pin_base, using sequential SMs
void link_init_ports(link_port_t *ports, uint8_t count,
                     uint8_t pin_base, uint8_t pio_idx, bool is_master);

// Poll all ports, return first with data (or -1)
int8_t link_poll_any(link_port_t *ports, uint8_t count,
                     pkt_header_t *hdr, uint8_t **payload);

#endif // PICOCLUSTER_LINK_H
