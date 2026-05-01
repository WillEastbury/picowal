#ifndef PICOCLUSTER_RING_H
#define PICOCLUSTER_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// ============================================================
// PIO Link Driver — Star topology point-to-point links
// ============================================================
//
// Workers: single link (2 GPIOs) to their head node
// Heads: 6 ports (12 GPIOs) + 1 interlink (2 GPIOs) = crossbar
//

// --- Worker API (single point-to-point link to head) ---
void link_worker_init(void);
bool link_worker_poll_rx(pkt_header_t *hdr, uint8_t **payload);
void link_worker_send(const pkt_header_t *hdr, const uint8_t *payload, uint16_t len);

// --- Head API (PIO switch fabric — 6 ports + interlink) ---
void link_head_init(void);

// Poll a specific port for incoming packet
bool link_head_poll_port(uint8_t port, pkt_header_t *hdr, uint8_t **payload);

// Send packet out a specific port
void link_head_send_port(uint8_t port, const pkt_header_t *hdr,
                         const uint8_t *payload, uint16_t len);

// Broadcast to all connected ports (except source)
void link_head_broadcast(const pkt_header_t *hdr, const uint8_t *payload,
                         uint16_t len, uint8_t except_port);

// Route a packet: look up dest -> port -> send (or interlink if not local)
bool link_head_route(const pkt_header_t *hdr, const uint8_t *payload, uint16_t len);

// Register a node ID on a port (called during discovery)
void link_head_set_port_node(uint8_t port, uint8_t node_id);

// Get port index for a node_id (0xFF if not found)
uint8_t link_head_find_port(uint8_t node_id);

#endif // PICOCLUSTER_RING_H
