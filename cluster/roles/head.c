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

#include <string.h>

// ============================================================
// Head Role — Network gateway + scheduler
// Core 0: W5500 network I/O + scheduling
// Core 1: Ring management + result collection
// ============================================================

#define MAX_WORKER_NODES    14
#define MAX_PENDING_JOBS    64

// --- Worker tracking ---
typedef struct {
    uint8_t  node_id;
    uint8_t  state;          // 0=idle, 1=busy, 2=offline
    uint8_t  queue_depth;
    uint16_t card_count;
    uint32_t last_seen;
    uint32_t bloom[8];       // Card residency bloom filter
} worker_info_t;

static worker_info_t g_workers[MAX_WORKER_NODES];
static uint8_t g_worker_count = 0;

// --- Pending job tracking ---
typedef struct {
    uint8_t  active;
    uint8_t  dest_node;
    uint8_t  card_major;
    uint8_t  card_minor;
    uint8_t  sock;           // Network socket waiting for response
    uint32_t timestamp;
} pending_job_t;

static pending_job_t g_jobs[MAX_PENDING_JOBS];

// --- Network config ---
static const w5500_net_config_t net_config = {
    .mac     = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
    .ip      = {192, 168, 1, 100},
    .gateway = {192, 168, 1, 1},
    .subnet  = {255, 255, 255, 0},
};

// --- Card cache (head also caches for fast distribution) ---
static card_cache_t g_card_cache;
static volatile uint8_t g_node_id = ADDR_MASTER;

// --- Ring config (same as worker) ---
static const ring_config_t head_ring_configs[RING_COUNT] = {
    { .pio_block = 0, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = RING0_PIN_TX, .pin_rx = RING0_PIN_RX,
      .dma_ch_tx = DMA_CH_RING0_TX, .dma_ch_rx = DMA_CH_RING0_RX,
      .baud_rate = RING_BAUD_RATE },
    { .pio_block = 0, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = RING1_PIN_TX, .pin_rx = RING1_PIN_RX,
      .dma_ch_tx = DMA_CH_RING1_TX, .dma_ch_rx = DMA_CH_RING1_RX,
      .baud_rate = RING_BAUD_RATE },
    { .pio_block = 1, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = RING2_PIN_TX, .pin_rx = RING2_PIN_RX,
      .dma_ch_tx = DMA_CH_RING2_TX, .dma_ch_rx = DMA_CH_RING2_RX,
      .baud_rate = RING_BAUD_RATE },
    { .pio_block = 1, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = RING3_PIN_TX, .pin_rx = RING3_PIN_RX,
      .dma_ch_tx = DMA_CH_RING3_TX, .dma_ch_rx = DMA_CH_RING3_RX,
      .baud_rate = RING_BAUD_RATE },
};

// --- Bloom filter helpers ---
static inline void bloom_set(uint32_t bloom[8], uint8_t major, uint8_t minor) {
    uint32_t h = (uint32_t)major * 31 + minor;
    bloom[h >> 5] |= (1u << (h & 31));
}

static inline bool bloom_test(const uint32_t bloom[8], uint8_t major, uint8_t minor) {
    uint32_t h = (uint32_t)major * 31 + minor;
    return (bloom[h >> 5] & (1u << (h & 31))) != 0;
}

// --- Scheduler ---
static uint8_t schedule_worker(uint8_t card_major, uint8_t card_minor) {
    uint8_t best = 0xFF;
    uint8_t best_score = 0xFF;

    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].state == 2) continue;  // Skip offline

        uint8_t score = g_workers[i].queue_depth;
        if (!bloom_test(g_workers[i].bloom, card_major, card_minor)) {
            score += 4;  // Cache miss penalty
        }

        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }

    return (best != 0xFF) ? g_workers[best].node_id : 1;
}

// --- Push card to ring if target likely doesn't have it ---
static void ensure_card_on_ring(uint8_t target, uint8_t major, uint8_t minor) {
    // Check target's bloom filter
    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].node_id == target) {
            if (bloom_test(g_workers[i].bloom, major, minor)) return;
            break;
        }
    }

    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache, major, minor, &card_len);
    if (!card) return;

    static uint8_t card_buf[PKT_MAX_PAYLOAD];
    pkt_card_data_t *cpkt = (pkt_card_data_t *)card_buf;
    cpkt->card_major = major;
    cpkt->card_minor = minor;
    cpkt->version = 1;
    cpkt->bytecode_len = card_len * 4;
    memcpy(cpkt->bytecode, card, card_len * 4);

    uint16_t plen = sizeof(pkt_card_data_t) + card_len * 4;
    pkt_header_t hdr = {
        .dest = ADDR_BROADCAST,
        .src = ADDR_MASTER,
        .type = PKT_CARD_DATA,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = plen,
    };
    hdr.crc16 = crc16_ccitt(card_buf, plen);
    ring_send(RING_NORMAL, &hdr, card_buf, plen);

    // Update all bloom filters
    for (uint8_t i = 0; i < g_worker_count; i++) {
        bloom_set(g_workers[i].bloom, major, minor);
    }
}

// --- Dispatch exec to cluster ---
static int dispatch_exec(uint8_t card_major, uint8_t card_minor,
                         const uint8_t *data, uint16_t data_len, uint8_t sock) {
    uint8_t target = schedule_worker(card_major, card_minor);
    ensure_card_on_ring(target, card_major, card_minor);

    static uint8_t exec_buf[PKT_MAX_PAYLOAD];
    pkt_exec_t *exec = (pkt_exec_t *)exec_buf;
    exec->card_major = card_major;
    exec->card_minor = card_minor;
    exec->data_len = data_len;
    if (data_len > 0) memcpy(exec->data, data, data_len);

    uint16_t plen = sizeof(pkt_exec_t) + data_len;
    pkt_header_t hdr = {
        .dest = target,
        .src = ADDR_MASTER,
        .type = PKT_EXEC,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
        .payload_len = plen,
    };
    hdr.crc16 = crc16_ccitt(exec_buf, plen);
    ring_send(RING_EXPRESS_1, &hdr, exec_buf, plen);

    // Track pending job
    for (int i = 0; i < MAX_PENDING_JOBS; i++) {
        if (!g_jobs[i].active) {
            g_jobs[i] = (pending_job_t){
                .active = 1, .dest_node = target,
                .card_major = card_major, .card_minor = card_minor,
                .sock = sock, .timestamp = time_us_32(),
            };
            return i;
        }
    }
    return -1;
}

// --- Handle result from worker ---
static void handle_ring_result(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_result_t *result = (const pkt_result_t *)payload;

    // Find matching pending job
    for (int i = 0; i < MAX_PENDING_JOBS; i++) {
        if (!g_jobs[i].active) continue;
        if (g_jobs[i].dest_node == hdr->src &&
            g_jobs[i].card_major == result->card_major &&
            g_jobs[i].card_minor == result->card_minor) {

            // Send result back via network
            if (result->data_len > 0) {
                w5500_socket_send(g_jobs[i].sock, result->data, result->data_len);
            }
            g_jobs[i].active = 0;

            // Update worker state
            for (uint8_t w = 0; w < g_worker_count; w++) {
                if (g_workers[w].node_id == hdr->src) {
                    if (g_workers[w].queue_depth > 0) g_workers[w].queue_depth--;
                    break;
                }
            }
            return;
        }
    }
}

// --- Handle NAK (cache miss) ---
static void handle_ring_nak(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_nak_t *nak = (const pkt_nak_t *)payload;
    ensure_card_on_ring(hdr->src, nak->card_major, nak->card_minor);
}

// --- Handle STATUS heartbeat ---
static void handle_ring_status(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_status_t *st = (const pkt_status_t *)payload;

    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].node_id == hdr->src) {
            g_workers[i].state = st->node_state;
            g_workers[i].queue_depth = st->queue_depth;
            g_workers[i].card_count = st->card_count;
            g_workers[i].last_seen = time_us_32();
            return;
        }
    }

    // New worker — add to list
    if (g_worker_count < MAX_WORKER_NODES) {
        worker_info_t *w = &g_workers[g_worker_count++];
        w->node_id = hdr->src;
        w->state = st->node_state;
        w->queue_depth = st->queue_depth;
        w->card_count = st->card_count;
        w->last_seen = time_us_32();
        memset(w->bloom, 0, sizeof(w->bloom));
    }
}

// --- Core 1: Ring polling ---
static void core1_ring_entry(void) {
    ring_init_all(head_ring_configs);
    ring_set_snoop_callback(NULL);  // Head doesn't snoop-cache

    while (1) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            // CRC
            uint16_t expected = hdr.crc16;
            if (hdr.payload_len > 0 &&
                crc16_ccitt(payload, hdr.payload_len) != expected) {
                ring_rx_done(r);
                continue;
            }

            if (hdr.dest == ADDR_MASTER || hdr.dest == ADDR_BROADCAST) {
                switch (hdr.type) {
                case PKT_RESULT:
                case PKT_BATCH_RES:
                    handle_ring_result(&hdr, payload);
                    break;
                case PKT_NAK:
                    handle_ring_nak(&hdr, payload);
                    break;
                case PKT_STATUS:
                    handle_ring_status(&hdr, payload);
                    break;
                default:
                    break;
                }
            }

            ring_rx_done(r);
        }
    }
}

// --- Core 0: Network I/O ---
static void core0_network_loop(void) {
    // Init W5500
    if (!w5500_init(&net_config)) {
        // Fatal: W5500 detected but init failed
        while (1) sleep_ms(1000);
    }

    // Open TCP listener on port 8080
    int listen_sock = w5500_socket_open(W5500_PROTO_TCP, 8080);
    if (listen_sock >= 0) {
        w5500_socket_listen((uint8_t)listen_sock);
    }

    // Open UDP socket on port 9000 (fast command channel)
    int udp_sock = w5500_socket_open(W5500_PROTO_UDP, 9000);

    // Run discovery
    discovery_run_master();

    // Main loop
    static uint8_t net_buf[PKT_MAX_PAYLOAD];

    while (1) {
        // Check TCP connections
        if (listen_sock >= 0 && w5500_socket_connected((uint8_t)listen_sock)) {
            int n = w5500_socket_recv((uint8_t)listen_sock, net_buf, sizeof(net_buf));
            if (n > 0) {
                // Parse: [card_major][card_minor][data...]
                if (n >= 2) {
                    uint8_t major = net_buf[0];
                    uint8_t minor = net_buf[1];
                    dispatch_exec(major, minor, net_buf + 2, (uint16_t)(n - 2),
                                  (uint8_t)listen_sock);
                }
            }
        }

        // Check UDP
        if (udp_sock >= 0) {
            uint8_t src_ip[4];
            uint16_t src_port;
            int n = w5500_socket_recvfrom((uint8_t)udp_sock, net_buf, sizeof(net_buf),
                                          src_ip, &src_port);
            if (n >= 2) {
                uint8_t major = net_buf[0];
                uint8_t minor = net_buf[1];
                dispatch_exec(major, minor, net_buf + 2, (uint16_t)(n - 2),
                              (uint8_t)udp_sock);
            }
        }

        // Timeout check for pending jobs
        uint32_t now = time_us_32();
        for (int i = 0; i < MAX_PENDING_JOBS; i++) {
            if (g_jobs[i].active && (now - g_jobs[i].timestamp) > 5000000) {
                // 5 second timeout — mark worker as slow/offline
                g_jobs[i].active = 0;
            }
        }

        sleep_us(100);  // Yield briefly
    }
}

// --- Entry point ---
void role_head_run(void) {
    card_cache_init(&g_card_cache);
    card_cache_warm_from_flash(&g_card_cache);

    g_node_id = ADDR_MASTER;

    multicore_launch_core1(core1_ring_entry);
    core0_network_loop();  // Never returns
}
