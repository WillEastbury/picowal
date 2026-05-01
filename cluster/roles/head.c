#include "roles.h"
#include "../common/config.h"
#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../drivers/w5500.h"
#include "../discovery/discovery.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"

#include <string.h>

// ============================================================
// Head Role — PIO switch fabric + network gateway + scheduler
// Core 0: Switch fabric (poll all 6 ports, route packets)
// Core 1: W5500 network I/O + scheduling logic
// ============================================================

// --- State ---
static card_cache_t g_card_cache;
static lease_table_t g_lease_table;

// --- Scheduler state ---
typedef struct {
    uint8_t  node_id;
    uint8_t  port;
    uint8_t  state;       // 0=idle, 1=busy
    uint8_t  queue_depth;
    uint16_t card_count;
    uint32_t exec_count;
    uint32_t last_seen;   // time_us_32
} node_info_t;

static node_info_t g_nodes[MAX_NODES];
static uint8_t g_node_count = 0;

// --- Find least-loaded idle worker ---
static uint8_t schedule_worker(void) {
    uint8_t best = 0xFF;
    uint8_t best_depth = 0xFF;

    for (uint8_t i = 0; i < g_node_count; i++) {
        if (g_nodes[i].state == 0 && g_nodes[i].queue_depth < best_depth) {
            best = g_nodes[i].node_id;
            best_depth = g_nodes[i].queue_depth;
        }
    }
    return best;
}

// --- Update node info from STATUS packet ---
static void update_node_status(uint8_t src_port, uint8_t src_id,
                               const pkt_status_t *st) {
    for (uint8_t i = 0; i < g_node_count; i++) {
        if (g_nodes[i].node_id == src_id) {
            g_nodes[i].state = st->node_state;
            g_nodes[i].queue_depth = st->queue_depth;
            g_nodes[i].card_count = st->card_count;
            g_nodes[i].exec_count = st->exec_count;
            g_nodes[i].last_seen = time_us_32();
            return;
        }
    }
    // New node
    if (g_node_count < MAX_NODES) {
        g_nodes[g_node_count] = (node_info_t){
            .node_id = src_id,
            .port = src_port,
            .state = st->node_state,
            .queue_depth = st->queue_depth,
            .card_count = st->card_count,
            .exec_count = st->exec_count,
            .last_seen = time_us_32(),
        };
        g_node_count++;
    }
}

// --- Core 0: PIO switch fabric ---
// Polls all 6 ports + interlink, routes packets by destination
static void core0_switch_loop(void) {
    link_head_init();

    // Run discovery — accept ADDR_REQ from nodes, assign IDs
    disc_master_init(&g_lease_table);

    // Discovery phase: poll ports and handle REQs until settled
    while (!disc_master_settled(&g_lease_table)) {
        watchdog_update();
        for (uint8_t p = 0; p <= PORTS_PER_HEAD; p++) {
            pkt_header_t hdr;
            uint8_t *payload;
            if (link_head_poll_port(p, &hdr, &payload)) {
                if (disc_master_handle(&g_lease_table, p, &hdr, payload)) {
                    // REQ handled — register port mapping
                    if (g_lease_table.count > 0) {
                        lease_entry_t *last = &g_lease_table.entries[g_lease_table.count - 1];
                        link_head_set_port_node(p, last->node_id);
                    }
                }
            }
        }
        sleep_ms(1);
    }

    // Main switching loop
    while (1) {
        watchdog_update();

        for (uint8_t p = 0; p <= PORTS_PER_HEAD; p++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!link_head_poll_port(p, &hdr, &payload)) continue;

            // Packets addressed to head (ADDR_MASTER = 0x00)
            if (hdr.dest == ADDR_MASTER) {
                switch (hdr.type) {
                case PKT_STATUS:
                    update_node_status(p, hdr.src, (pkt_status_t *)payload);
                    break;
                case PKT_RESULT:
                    // Store result for network egress (Core 1 picks up)
                    // TODO: intercore result queue
                    break;
                case PKT_NAK:
                    // Node needs a card — serve from cache or SD
                    {
                        const pkt_nak_t *nak = (const pkt_nak_t *)payload;
                        uint32_t card_len;
                        uint32_t *card = card_cache_get(&g_card_cache,
                                                        nak->card_major, nak->card_minor,
                                                        &card_len);
                        if (card) {
                            // Build CARD_DATA and send to requesting port
                            static uint8_t card_pkt[PKT_MAX_PAYLOAD];
                            pkt_card_data_t *cpkt = (pkt_card_data_t *)card_pkt;
                            cpkt->card_major = nak->card_major;
                            cpkt->card_minor = nak->card_minor;
                            cpkt->version = 1;
                            cpkt->bytecode_len = card_len;
                            memcpy(cpkt->bytecode, card, card_len);

                            uint16_t plen = sizeof(pkt_card_data_t) + card_len;
                            pkt_header_t chdr = {
                                .dest = hdr.src,
                                .src = ADDR_MASTER,
                                .type = PKT_CARD_DATA,
                                .flags = 0,
                                .payload_len = plen,
                            };
                            chdr.crc16 = crc16_ccitt(card_pkt, plen);
                            link_head_send_port(p, &chdr, card_pkt, plen);
                        }
                    }
                    break;
                case PKT_ADDR_REQ:
                    // Late discovery REQ (node rebooted)
                    if (disc_master_handle(&g_lease_table, p, &hdr, payload)) {
                        lease_entry_t *last = &g_lease_table.entries[g_lease_table.count - 1];
                        link_head_set_port_node(p, last->node_id);
                    }
                    break;
                default:
                    break;
                }
            } else {
                // Route to destination (another node or broadcast)
                link_head_route(&hdr, payload, hdr.payload_len);
            }
        }
    }
}

// --- Core 1: Network I/O + job dispatch ---
static void core1_network(void) {
    w5500_init();
    card_cache_init(&g_card_cache);
    card_cache_warm_from_flash(&g_card_cache);

    // Open UDP socket for job ingress
    w5500_socket_open(0, W5500_PROTO_UDP, 8002);

    static uint8_t net_buf[PKT_MAX_PAYLOAD];

    while (1) {
        watchdog_update();

        // Receive network packets
        uint16_t len = w5500_socket_recv(0, net_buf, sizeof(net_buf));
        if (len > 0) {
            // Parse as EXEC request, schedule to a worker
            uint8_t target = schedule_worker();
            if (target != 0xFF) {
                pkt_header_t hdr = {
                    .dest = target,
                    .src = ADDR_MASTER,
                    .type = PKT_EXEC,
                    .flags = 0,
                    .payload_len = len,
                };
                hdr.crc16 = crc16_ccitt(net_buf, len);
                link_head_route(&hdr, net_buf, len);
            }
        }

        sleep_us(10);
    }
}

// --- Entry point ---
void role_head_run(void) {
    multicore_launch_core1(core1_network);
    core0_switch_loop();
}
