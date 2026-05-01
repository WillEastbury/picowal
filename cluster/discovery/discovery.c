#include "discovery.h"
#include "../common/config.h"
#include "../common/packet.h"
#include "../common/ring.h"

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include <string.h>

// ============================================================
// DHCP-style Discovery Protocol
// ============================================================
//
// Simple REQ → ACK model:
// - Nodes use RP2350 unique board ID as hardware identifier
// - Head assigns sequential IDs on first-come basis
// - Works across multiple rings (REQs forwarded through ring)
// - Nodes keep retrying until ACK received or timeout
//

// Packet types (extend PKT_* namespace)
#define PKT_ADDR_REQ  0x10
#define PKT_ADDR_ACK  0x11

// --- Helper: get this board's unique ID ---
void disc_get_hw_id(hw_id_t *out) {
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    memcpy(out->bytes, uid.id, 8);
}

static bool hw_id_match(const hw_id_t *a, const hw_id_t *b) {
    return memcmp(a->bytes, b->bytes, 8) == 0;
}

// ============================================================
// HEAD (Master) side
// ============================================================

void disc_master_init(lease_table_t *table) {
    memset(table, 0, sizeof(*table));
    table->next_id = 1;  // Node IDs start at 1 (0 = head)
}

static uint8_t find_or_assign(lease_table_t *table, const hw_id_t *hw_id, uint8_t role) {
    // Check if already assigned
    for (uint8_t i = 0; i < table->count; i++) {
        if (table->entries[i].active && hw_id_match(&table->entries[i].hw_id, hw_id)) {
            return table->entries[i].node_id;
        }
    }

    // Assign new
    if (table->count >= MAX_CLUSTER_NODES) return 0;  // Full

    lease_entry_t *entry = &table->entries[table->count];
    entry->hw_id = *hw_id;
    entry->node_id = table->next_id++;
    entry->role = role;
    entry->active = true;
    table->count++;

    return entry->node_id;
}

static uint32_t last_req_time = 0;

bool disc_master_handle(lease_table_t *table, uint8_t ring,
                        const void *hdr_ptr, const uint8_t *payload) {
    const pkt_header_t *hdr = (const pkt_header_t *)hdr_ptr;

    if (hdr->type != PKT_ADDR_REQ) return false;
    if (hdr->payload_len < sizeof(addr_req_t)) return false;

    const addr_req_t *req = (const addr_req_t *)payload;

    // Assign (or re-confirm) an ID
    uint8_t assigned = find_or_assign(table, &req->hw_id, req->role);
    if (assigned == 0) return false;  // Table full

    last_req_time = time_us_32();

    // Send ACK back on all rings (broadcast so other nodes forward it)
    addr_ack_t ack = {
        .hw_id = req->hw_id,
        .assigned_id = assigned,
        .flags = 0,
    };

    pkt_header_t ack_hdr = {
        .dest = ADDR_BROADCAST,
        .src = ADDR_MASTER,
        .type = PKT_ADDR_ACK,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 2),
        .payload_len = sizeof(ack),
    };
    ack_hdr.crc16 = crc16_ccitt((const uint8_t *)&ack, sizeof(ack));

    for (uint8_t r = 0; r < RING_COUNT; r++) {
        ring_send(r, &ack_hdr, (const uint8_t *)&ack, sizeof(ack));
    }

    return true;
}

bool disc_master_settled(const lease_table_t *table) {
    if (table->count == 0) return false;
    uint32_t elapsed = (time_us_32() - last_req_time) / 1000;
    return elapsed >= DISC_SETTLE_MS;
}

// ============================================================
// PARTICIPANT (Worker/Storage) side
// ============================================================

uint8_t disc_participant_run(uint8_t my_role) {
    hw_id_t my_hw_id;
    disc_get_hw_id(&my_hw_id);

    // Build REQ packet
    addr_req_t req = {
        .hw_id = my_hw_id,
        .role = my_role,
        .flags = 0,
    };

    pkt_header_t req_hdr = {
        .dest = ADDR_MASTER,
        .src = 0xFE,  // Unaddressed
        .type = PKT_ADDR_REQ,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
        .payload_len = sizeof(req),
    };
    req_hdr.crc16 = crc16_ccitt((const uint8_t *)&req, sizeof(req));

    uint64_t deadline = time_us_64() + (uint64_t)DISC_TIMEOUT_MS * 1000;
    uint64_t next_req = 0;

    while (time_us_64() < deadline) {
        // Send/resend REQ periodically
        if (time_us_64() >= next_req) {
            // Send on all rings
            for (uint8_t r = 0; r < RING_COUNT; r++) {
                ring_send(r, &req_hdr, (const uint8_t *)&req, sizeof(req));
            }
            next_req = time_us_64() + (uint64_t)DISC_REQ_INTERVAL_MS * 1000;
        }

        // Poll for ACK
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t rx_hdr;
            uint8_t *rx_payload;

            if (!ring_poll_rx(r, &rx_hdr, &rx_payload)) continue;

            if (rx_hdr.type == PKT_ADDR_ACK && rx_hdr.src == ADDR_MASTER) {
                const addr_ack_t *ack = (const addr_ack_t *)rx_payload;

                if (hw_id_match(&ack->hw_id, &my_hw_id)) {
                    // This ACK is for us!
                    ring_forward(r, r);  // Forward for other nodes
                    ring_rx_done(r);
                    return ack->assigned_id;
                }

                // ACK for someone else — forward it
                ring_forward(r, r);
            } else if (rx_hdr.type == PKT_ADDR_REQ && rx_hdr.src != 0xFE) {
                // Another node's REQ — forward it
                ring_forward(r, r);
            } else {
                // Forward everything during discovery
                ring_forward(r, r);
            }

            ring_rx_done(r);
        }

        sleep_us(50);
    }

    // Timeout — no head found, return unassigned
    return 0;
}
