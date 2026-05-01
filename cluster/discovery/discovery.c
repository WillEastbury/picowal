#include "discovery.h"
#include "../common/config.h"
#include "../common/packet.h"
#include "../common/ring.h"

#include "pico/stdlib.h"
#include <string.h>

// ============================================================
// Discovery Protocol
// ============================================================
//
// 1. Head broadcasts DISCOVER on all rings (TTL=15)
// 2. Each node receives DISCOVER:
//    - Records hop count (TTL_initial - TTL_remaining)
//    - Responds with STATUS (includes role, capabilities)
//    - Forwards DISCOVER downstream (TTL--)
// 3. Head collects all STATUS responses within timeout
// 4. Head assigns sequential IDs based on ring position
// 5. Head broadcasts ROUTE packet with ID assignments
// 6. Nodes read their assigned ID from ROUTE packet
//
// Packet payload for DISCOVER: [sequence:4]
// Packet payload for ROUTE: [count:1][node_entries: id,position × N]

#define DISC_SEQUENCE_MAGIC  0xD15C0000

static discovery_result_t g_disc_result;

// --- Master: run full discovery ---
void discovery_run_master(void) {
    memset(&g_disc_result, 0, sizeof(g_disc_result));
    g_disc_result.state = DISC_ANNOUNCING;

    // Send DISCOVER on all rings
    uint32_t seq = DISC_SEQUENCE_MAGIC | (time_us_32() & 0xFFFF);

    pkt_header_t hdr = {
        .dest = ADDR_BROADCAST,
        .src = ADDR_MASTER,
        .type = PKT_DISCOVER,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 2),  // High priority
        .payload_len = 4,
    };

    uint8_t payload[4];
    memcpy(payload, &seq, 4);
    hdr.crc16 = crc16_ccitt(payload, 4);

    for (uint8_t r = 0; r < RING_COUNT; r++) {
        ring_send(r, &hdr, payload, 4);
    }

    // Collect responses for DISCOVERY_TIMEOUT_MS
    uint64_t deadline = time_us_64() + (uint64_t)DISCOVERY_TIMEOUT_MS * 1000;
    g_disc_result.state = DISC_LEARNING;

    while (time_us_64() < deadline) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t rx_hdr;
            uint8_t *rx_payload;

            if (!ring_poll_rx(r, &rx_hdr, &rx_payload)) continue;

            if (rx_hdr.type == PKT_STATUS && rx_hdr.dest == ADDR_MASTER) {
                // New node responding
                if (g_disc_result.node_count < MAX_DISCOVERED_NODES) {
                    const pkt_status_t *st = (const pkt_status_t *)rx_payload;
                    discovered_node_t *node = &g_disc_result.nodes[g_disc_result.node_count];
                    node->node_id = rx_hdr.src;  // Temporary ID (or 0xFE if unassigned)
                    node->role = st->node_state; // Overloaded: use status for role during discovery
                    node->ring_position = DEFAULT_TTL - PKT_TTL(rx_hdr.flags);
                    node->flags = 0;
                    g_disc_result.node_count++;
                }
            }

            ring_rx_done(r);
        }

        sleep_us(100);
    }

    // Assign IDs based on ring position (closer to head = lower ID)
    // Simple: sort by ring_position, assign 1..N
    for (uint8_t i = 0; i < g_disc_result.node_count; i++) {
        for (uint8_t j = i + 1; j < g_disc_result.node_count; j++) {
            if (g_disc_result.nodes[j].ring_position < g_disc_result.nodes[i].ring_position) {
                discovered_node_t tmp = g_disc_result.nodes[i];
                g_disc_result.nodes[i] = g_disc_result.nodes[j];
                g_disc_result.nodes[j] = tmp;
            }
        }
        g_disc_result.nodes[i].node_id = i + 1;  // Assign 1-based ID
    }

    // Broadcast ROUTE with ID assignments
    // Format: [count][id, position, id, position, ...]
    uint8_t route_buf[1 + MAX_DISCOVERED_NODES * 2];
    route_buf[0] = g_disc_result.node_count;
    for (uint8_t i = 0; i < g_disc_result.node_count; i++) {
        route_buf[1 + i * 2] = g_disc_result.nodes[i].node_id;
        route_buf[2 + i * 2] = g_disc_result.nodes[i].ring_position;
    }

    uint16_t route_len = 1 + g_disc_result.node_count * 2;
    pkt_header_t route_hdr = {
        .dest = ADDR_BROADCAST,
        .src = ADDR_MASTER,
        .type = PKT_ROUTE,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 2),
        .payload_len = route_len,
    };
    route_hdr.crc16 = crc16_ccitt(route_buf, route_len);

    for (uint8_t r = 0; r < RING_COUNT; r++) {
        ring_send(r, &route_hdr, route_buf, route_len);
    }

    g_disc_result.state = DISC_COMPLETE;
}

// --- Participant: respond to discovery ---
void discovery_run_participant(discovery_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->state = DISC_IDLE;

    // Wait for DISCOVER packet (with timeout)
    uint64_t deadline = time_us_64() + (uint64_t)DISCOVERY_TIMEOUT_MS * 1000;

    while (time_us_64() < deadline) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            if (hdr.type == PKT_DISCOVER && hdr.src == ADDR_MASTER) {
                // Respond with STATUS
                uint8_t my_position = DEFAULT_TTL - PKT_TTL(hdr.flags);
                result->my_position = my_position;
                result->state = DISC_LEARNING;

                pkt_status_t status = {
                    .node_state = 0,
                    .queue_depth = 0,
                    .card_count = 0,
                    .exec_count = 0,
                    .uptime_sec = time_us_32() / 1000000,
                };

                pkt_header_t resp = {
                    .dest = ADDR_MASTER,
                    .src = 0xFE,  // Unassigned
                    .type = PKT_STATUS,
                    .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
                    .payload_len = sizeof(status),
                };
                resp.crc16 = crc16_ccitt((uint8_t *)&status, sizeof(status));
                ring_send(r, &resp, (uint8_t *)&status, sizeof(status));

                // Forward DISCOVER
                ring_forward(r, r);
                ring_rx_done(r);

                // Now wait for ROUTE packet
                goto wait_for_route;
            }

            // Forward any non-discovery packets normally
            ring_rx_done(r);
        }
        sleep_us(100);
    }

    // Timeout — no discovery happened
    result->state = DISC_IDLE;
    result->assigned_id = 0xFE;
    return;

wait_for_route:
    deadline = time_us_64() + (uint64_t)DISCOVERY_TIMEOUT_MS * 1000;

    while (time_us_64() < deadline) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            if (hdr.type == PKT_ROUTE && hdr.src == ADDR_MASTER) {
                // Find our ID by position
                uint8_t count = payload[0];
                for (uint8_t i = 0; i < count; i++) {
                    uint8_t id = payload[1 + i * 2];
                    uint8_t pos = payload[2 + i * 2];
                    if (pos == result->my_position) {
                        result->assigned_id = id;
                        result->state = DISC_COMPLETE;
                        ring_forward(r, r);  // Pass along
                        ring_rx_done(r);
                        return;
                    }
                }
                ring_forward(r, r);
            }

            ring_rx_done(r);
        }
        sleep_us(100);
    }

    result->assigned_id = 0xFE;
    result->state = DISC_COMPLETE;
}

uint8_t discovery_get_id(void) {
    return g_disc_result.assigned_id;
}
