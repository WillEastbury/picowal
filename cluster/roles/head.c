#include "roles.h"
#include "../common/config.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../drivers/w5500.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"

#include <string.h>

// ============================================================
// Head Role — PIO switch + scheduler
// GP0-GP11: 12 half-duplex ports. ports[0] IS node 0. Done.
// Hotplug: just ping each port. If it replies, it's alive.
// ============================================================

static link_port_t g_ports[HEAD_PORT_COUNT];
static card_cache_t g_card_cache;

// --- Per-port state (the port index IS the node ID) ---
typedef struct {
    bool     alive;        // Responded to last ping
    bool     busy;         // Currently executing
    uint8_t  queue_depth;
    uint16_t card_count;
    uint32_t exec_count;
    uint32_t last_seen;    // time_us_32 of last response
    uint32_t clock_khz;    // Current clock speed of this worker
} port_state_t;

static port_state_t g_state[HEAD_PORT_COUNT];

// --- Power scaling: clock workers up/down based on load ---
static void clock_set_port(uint8_t port, uint32_t khz) {
    pkt_clock_set_t clk = { .target_khz = khz };
    pkt_header_t hdr = {
        .type = PKT_CLOCK_SET,
        .payload_len = sizeof(clk),
        .crc16 = crc16_ccitt((uint8_t *)&clk, sizeof(clk)),
    };
    link_send(&g_ports[port], &hdr, (uint8_t *)&clk, sizeof(clk));
}

// Scale up a worker before dispatching work
static void ensure_clocked_up(uint8_t port) {
    if (g_state[port].clock_khz < SYS_CLOCK_MAX_KHZ) {
        clock_set_port(port, SYS_CLOCK_MAX_KHZ);
        g_state[port].clock_khz = SYS_CLOCK_MAX_KHZ;
    }
}

// Scale down idle workers (called periodically)
static void scale_down_idle(uint32_t now) {
    for (uint8_t p = 0; p < HEAD_PORT_COUNT; p++) {
        if (!g_state[p].alive) continue;
        if (g_state[p].busy) continue;
        uint32_t idle_us = now - g_state[p].last_seen;
        if (idle_us > 1000000 && g_state[p].clock_khz > SYS_CLOCK_IDLE_KHZ) {
            // Idle > 1s → drop to 12 MHz
            clock_set_port(p, SYS_CLOCK_IDLE_KHZ);
            g_state[p].clock_khz = SYS_CLOCK_IDLE_KHZ;
        }
    }
}

// --- Probe all ports (ping/pong) ---
static void probe_ports(void) {
    pkt_header_t ping = {
        .type = PKT_PING,
        .payload_len = 0,
        .crc16 = 0,
    };

    for (uint8_t p = 0; p < HEAD_PORT_COUNT; p++) {
        pkt_header_t reply;
        uint8_t *reply_payload;

        if (link_transact(&g_ports[p], &ping, NULL, 0,
                          &reply, &reply_payload, 5000)) {
            g_state[p].alive = true;
            g_state[p].last_seen = time_us_32();
            if (g_state[p].clock_khz == 0) g_state[p].clock_khz = SYS_CLOCK_MAX_KHZ;
            if (reply.type == PKT_STATUS && reply.payload_len >= sizeof(pkt_status_t)) {
                const pkt_status_t *st = (const pkt_status_t *)reply_payload;
                g_state[p].busy = st->busy;
                g_state[p].queue_depth = st->queue_depth;
                g_state[p].card_count = st->card_count;
                g_state[p].exec_count = st->exec_count;
            }
        } else {
            g_state[p].alive = false;
        }
    }
}

// --- Find least-loaded live port ---
static int8_t pick_worker(void) {
    int8_t best = -1;
    uint8_t best_depth = 0xFF;

    for (uint8_t p = 0; p < HEAD_PORT_COUNT; p++) {
        if (g_state[p].alive && !g_state[p].busy &&
            g_state[p].queue_depth < best_depth) {
            best = p;
            best_depth = g_state[p].queue_depth;
        }
    }
    return best;
}

// --- Dispatch exec to a worker port ---
static bool dispatch_exec(const uint8_t *data, uint16_t len) {
    int8_t port = pick_worker();
    if (port < 0) return false;

    ensure_clocked_up(port);

    pkt_header_t hdr = {
        .type = PKT_EXEC,
        .payload_len = len,
        .crc16 = crc16_ccitt(data, len),
    };

    link_send(&g_ports[port], &hdr, data, len);
    g_state[port].busy = true;
    return true;
}

// --- Core 0: Poll ports for results, handle hotplug ---
static void core0_switch_loop(void) {
    link_init_ports(g_ports, HEAD_PORT_COUNT, HEAD_PORT_BASE, 0, true);
    memset(g_state, 0, sizeof(g_state));

    // Initial probe
    probe_ports();

    uint32_t last_probe = time_us_32();

    while (1) {
        watchdog_update();
        uint32_t now = time_us_32();

        // Poll all ports for incoming results/status
        for (uint8_t p = 0; p < HEAD_PORT_COUNT; p++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (link_poll(&g_ports[p], &hdr, &payload)) {
                g_state[p].last_seen = now;
                g_state[p].alive = true;

                switch (hdr.type) {
                case PKT_RESULT:
                    g_state[p].busy = false;
                    // TODO: forward result to network (Core 1 picks up)
                    break;
                case PKT_STATUS:
                    if (hdr.payload_len >= sizeof(pkt_status_t)) {
                        const pkt_status_t *st = (const pkt_status_t *)payload;
                        g_state[p].busy = st->busy;
                        g_state[p].queue_depth = st->queue_depth;
                        g_state[p].card_count = st->card_count;
                    }
                    break;
                case PKT_PONG:
                    // Alive confirmation (hotplug detection)
                    break;
                default:
                    break;
                }
            }
        }

        // Periodic re-probe (detect hotplug/unplug, every 2s)
        if (now - last_probe > 2000000) {
            last_probe = now;
            probe_ports();
            scale_down_idle(now);
        }
    }
}

// --- Core 1: Network I/O ---
static void core1_network(void) {
    w5500_init();
    card_cache_init(&g_card_cache);
    card_cache_warm_from_flash(&g_card_cache);

    w5500_socket_open(0, W5500_PROTO_UDP, 8002);

    static uint8_t net_buf[PKT_MAX_PAYLOAD];

    while (1) {
        watchdog_update();

        uint16_t len = w5500_socket_recv(0, net_buf, sizeof(net_buf));
        if (len > 0) {
            dispatch_exec(net_buf, len);
        }

        sleep_us(10);
    }
}

// --- Entry point ---
void role_head_run(void) {
    multicore_launch_core1(core1_network);
    core0_switch_loop();
}
