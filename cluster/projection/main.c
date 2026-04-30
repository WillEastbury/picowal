// ============================================================
// PicoCluster Projection Node — Gateway + Scheduler
// ============================================================
// Pico2W + W5500: Network I/O, SD storage, data prefetch,
// card distribution, work scheduling
// ============================================================

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"

#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"

// ============================================================
// Configuration
// ============================================================

#define OVERCLOCK_KHZ       450000
#define MAX_WORKER_NODES    14
#define SCHEDULER_TICK_US   100

// W5500 SPI config
#define W5500_SPI           spi0
#define W5500_SPI_BAUD      40000000  // 40 MHz SPI
#define W5500_PIN_MISO      16
#define W5500_PIN_CS        17
#define W5500_PIN_SCK       18
#define W5500_PIN_MOSI      19
#define W5500_PIN_RST       20
#define W5500_PIN_INT       21

// SD card SPI config (second SD on spi1)
#define SD_SPI              spi1
#define SD_SPI_BAUD         25000000  // 25 MHz
#define SD_PIN_MISO         12
#define SD_PIN_CS           13
#define SD_PIN_SCK          14
#define SD_PIN_MOSI         15

// ============================================================
// Node state tracking
// ============================================================

typedef enum {
    WORKER_UNKNOWN = 0,
    WORKER_READY,
    WORKER_BUSY,
    WORKER_ERROR,
    WORKER_OFFLINE,
} worker_state_t;

typedef struct {
    uint8_t        node_id;
    worker_state_t state;
    uint32_t       last_heartbeat;   // Timestamp of last STATUS
    uint8_t        queue_depth;      // Pending exec requests
    uint16_t       card_count;       // Known cached cards
    uint32_t       exec_count;       // Total executions
    // Bloom filter for card residency (approximate)
    uint32_t       card_bloom[8];    // 256-bit bloom filter
} worker_info_t;

static worker_info_t g_workers[MAX_WORKER_NODES];
static uint8_t g_worker_count = 0;

// Card storage (full card store on SD)
static card_cache_t g_card_cache;  // SRAM mirror for hot cards

// Ring configs (same as worker, projection is on the ring too)
static const ring_config_t ring_configs[RING_COUNT] = {
    { .pio_block = 0, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = 0, .pin_rx = 1, .dma_ch_tx = 0, .dma_ch_rx = 1,
      .baud_rate = 20000000 },
    { .pio_block = 0, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = 2, .pin_rx = 3, .dma_ch_tx = 2, .dma_ch_rx = 3,
      .baud_rate = 20000000 },
    { .pio_block = 1, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = 4, .pin_rx = 5, .dma_ch_tx = 4, .dma_ch_rx = 5,
      .baud_rate = 20000000 },
    { .pio_block = 1, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = 6, .pin_rx = 7, .dma_ch_tx = 6, .dma_ch_rx = 7,
      .baud_rate = 20000000 },
};

// ============================================================
// Bloom filter helpers (for card residency tracking)
// ============================================================

static inline void bloom_set(uint32_t bloom[8], uint8_t major, uint8_t minor) {
    uint32_t h = (uint32_t)major * 31 + minor;
    bloom[h >> 5] |= (1u << (h & 31));
}

static inline bool bloom_test(uint32_t bloom[8], uint8_t major, uint8_t minor) {
    uint32_t h = (uint32_t)major * 31 + minor;
    return (bloom[h >> 5] & (1u << (h & 31))) != 0;
}

// ============================================================
// Scheduler — pick best worker for a card execution
// ============================================================

static uint8_t schedule_worker(uint8_t card_major, uint8_t card_minor) {
    uint8_t best = 0xFF;
    uint8_t best_depth = 0xFF;

    for (uint8_t i = 0; i < g_worker_count; i++) {
        worker_info_t *w = &g_workers[i];
        if (w->state != WORKER_READY) continue;

        // Prefer workers that likely have the card cached
        bool has_card = bloom_test(w->card_bloom, card_major, card_minor);

        // Prefer least loaded
        uint8_t score = w->queue_depth;
        if (!has_card) score += 4;  // Penalty for likely cache miss

        if (score < best_depth) {
            best_depth = score;
            best = i;
        }
    }

    return (best != 0xFF) ? g_workers[best].node_id : 1;  // Fallback to node 1
}

// ============================================================
// Card distribution — push card to ring if needed
// ============================================================

static void ensure_card_available(uint8_t target_node, uint8_t major, uint8_t minor) {
    // Check bloom filter — does target likely have it?
    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].node_id == target_node) {
            if (bloom_test(g_workers[i].card_bloom, major, minor)) {
                return;  // Likely cached, skip push
            }
            break;
        }
    }

    // Push card to ring (all nodes will snoop and cache)
    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache, major, minor, &card_len);
    if (!card) {
        // TODO: load from SD card
        return;
    }

    pkt_header_t hdr = {
        .dest = ADDR_BROADCAST,
        .src = ADDR_MASTER,
        .type = PKT_CARD_DATA,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = sizeof(pkt_card_data_t) + card_len * 4,
    };

    // Build card packet
    static uint8_t card_pkt_buf[PKT_MAX_PAYLOAD];
    pkt_card_data_t *cpkt = (pkt_card_data_t *)card_pkt_buf;
    cpkt->card_major = major;
    cpkt->card_minor = minor;
    cpkt->version = 1;  // TODO: versioning
    cpkt->bytecode_len = card_len * 4;
    memcpy(cpkt->bytecode, card, card_len * 4);

    hdr.crc16 = crc16_ccitt(card_pkt_buf, hdr.payload_len);
    ring_send(RING_NORMAL, &hdr, card_pkt_buf, hdr.payload_len);

    // Update bloom filter for all nodes (broadcast = all get it)
    for (uint8_t i = 0; i < g_worker_count; i++) {
        bloom_set(g_workers[i].card_bloom, major, minor);
    }
}

// ============================================================
// Dispatch execution to cluster
// ============================================================

void dispatch_exec(uint8_t card_major, uint8_t card_minor,
                   const uint8_t *data, uint16_t data_len) {
    // Schedule to best worker
    uint8_t target = schedule_worker(card_major, card_minor);

    // Ensure card is available
    ensure_card_available(target, card_major, card_minor);

    // Send EXEC packet
    static uint8_t exec_buf[PKT_MAX_PAYLOAD];
    pkt_exec_t *exec = (pkt_exec_t *)exec_buf;
    exec->card_major = card_major;
    exec->card_minor = card_minor;
    exec->data_len = data_len;
    if (data_len > 0) {
        memcpy(exec->data, data, data_len);
    }

    pkt_header_t hdr = {
        .dest = target,
        .src = ADDR_MASTER,
        .type = PKT_EXEC,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
        .payload_len = sizeof(pkt_exec_t) + data_len,
    };
    hdr.crc16 = crc16_ccitt(exec_buf, hdr.payload_len);

    // Use express ring for low-latency dispatch
    ring_send(RING_EXPRESS_1, &hdr, exec_buf, hdr.payload_len);

    // Update worker state
    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].node_id == target) {
            g_workers[i].queue_depth++;
            break;
        }
    }
}

// ============================================================
// Handle incoming results from workers
// ============================================================

static void handle_result(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_result_t *result = (const pkt_result_t *)payload;

    // Update worker state
    for (uint8_t i = 0; i < g_worker_count; i++) {
        if (g_workers[i].node_id == hdr->src) {
            if (g_workers[i].queue_depth > 0) g_workers[i].queue_depth--;
            g_workers[i].exec_count++;
            break;
        }
    }

    // Forward result to network (W5500 or WiFi)
    // TODO: W5500 driver integration
    (void)result;
}

// ============================================================
// Handle NAKs (card cache miss on worker)
// ============================================================

static void handle_nak(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_nak_t *nak = (const pkt_nak_t *)payload;

    // Worker doesn't have this card — push it
    ensure_card_available(hdr->src, nak->card_major, nak->card_minor);
}

// ============================================================
// Core 0: Network I/O (W5500 + WiFi)
// Core 1: Ring management + scheduling
// ============================================================

static void core1_ring_loop(void) {
    ring_init_all(ring_configs);

    while (1) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            // Verify CRC
            uint16_t expected_crc = hdr.crc16;
            hdr.crc16 = 0;
            if (crc16_ccitt(payload, hdr.payload_len) != expected_crc) {
                ring_rx_done(r);
                continue;
            }

            // Process
            if (hdr.dest == ADDR_MASTER || hdr.dest == ADDR_BROADCAST) {
                switch (hdr.type) {
                case PKT_RESULT:
                case PKT_BATCH_RES:
                    handle_result(&hdr, payload);
                    break;
                case PKT_NAK:
                    handle_nak(&hdr, payload);
                    break;
                case PKT_STATUS: {
                    const pkt_status_t *status = (const pkt_status_t *)payload;
                    for (uint8_t i = 0; i < g_worker_count; i++) {
                        if (g_workers[i].node_id == hdr.src) {
                            g_workers[i].state = (status->node_state == 0) ?
                                WORKER_READY : WORKER_BUSY;
                            g_workers[i].queue_depth = status->queue_depth;
                            g_workers[i].card_count = status->card_count;
                            g_workers[i].last_heartbeat = time_us_32();
                            break;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            ring_rx_done(r);
        }
    }
}

// ============================================================
// Entry point
// ============================================================

int main(void) {
    set_sys_clock_khz(OVERCLOCK_KHZ, true);
    stdio_init_all();

    // Init WiFi
    if (cyw43_arch_init()) {
        // WiFi init failed — continue without it
    }

    // Init card cache
    card_cache_init(&g_card_cache);
    card_cache_warm_from_flash(&g_card_cache);

    // TODO: Init W5500 via SPI
    // TODO: Init SD card via SPI
    // TODO: Load cards from SD into cache

    // Launch Core 1 for ring management
    multicore_launch_core1(core1_ring_loop);

    // Core 0: Network I/O main loop
    while (1) {
        // TODO: W5500 poll for incoming network requests
        // TODO: Parse HTTP/TCP/UDP → dispatch_exec()
        // TODO: Collect results → send back via W5500

        // WiFi management
        cyw43_arch_poll();

        sleep_us(SCHEDULER_TICK_US);
    }

    return 0;
}
