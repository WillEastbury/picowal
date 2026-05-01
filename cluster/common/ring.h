#ifndef PICOCLUSTER_LINK_H
#define PICOCLUSTER_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// ============================================================
// Half-duplex single-wire PIO link
// ============================================================
// Point-to-point. No addresses. Send a command, get a reply.
// Physical wire = identity. Head knows port 3 = worker 3.

typedef struct {
    uint8_t  pin;
    uint8_t  sm;
    uint8_t  pio_idx;
    uint8_t  dma_ch;
} link_port_t;

// --- Init ---
void link_init_port(link_port_t *port, bool start_as_listener);
void link_init_ports(link_port_t *ports, uint8_t count,
                     uint8_t pin_base, uint8_t pio_idx, bool start_as_listener);

// --- Send a packet (blocks until sent, then switches to listen) ---
void link_send(link_port_t *port, const pkt_header_t *hdr,
               const uint8_t *payload, uint16_t len);

// --- Poll for incoming packet (non-blocking) ---
bool link_poll(link_port_t *port, pkt_header_t *hdr, uint8_t **payload);

// --- Send and wait for reply (blocking with timeout) ---
bool link_transact(link_port_t *port,
                   const pkt_header_t *send_hdr, const uint8_t *send_payload, uint16_t send_len,
                   pkt_header_t *reply_hdr, uint8_t **reply_payload,
                   uint32_t timeout_us);

// --- Poll N ports, return index of first with data (or -1) ---
int8_t link_poll_any(link_port_t *ports, uint8_t count,
                     pkt_header_t *hdr, uint8_t **payload);

#endif // PICOCLUSTER_LINK_H
