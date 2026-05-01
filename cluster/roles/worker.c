#include "roles.h"
#include "../common/config.h"
#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"

#include <string.h>

// ============================================================
// Worker Role — Pure compute node
// Core 0: Ring relay + bus snooping + dispatch
// Core 1: VM execution
// ============================================================

// --- Memory regions ---
static uint8_t __attribute__((aligned(4))) vm_data_mem[MEM_VM_DATA_SIZE];
static uint8_t __attribute__((aligned(4))) vm_stack_mem[MEM_VM_STACK_SIZE];
static uint8_t __attribute__((aligned(4))) result_buf[MEM_RESULT_BUF_SIZE];

// --- Shared state ---
static card_cache_t g_card_cache;
static vm_context_t g_vm_ctx;
static volatile uint8_t g_node_id = 0xFF;

// --- Exec queue (Core 0 → Core 1) ---
typedef struct {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t data_len;
    uint8_t  src_node;
    uint8_t  ring_id;
    uint32_t data_offset;  // Offset into vm_data_mem
} exec_request_t;

static volatile exec_request_t g_exec_queue[VM_EXEC_QUEUE_SIZE];
static volatile uint8_t g_exec_head = 0;
static volatile uint8_t g_exec_tail = 0;
static volatile uint32_t g_data_write_offset = 0;

// --- Ring config ---
static const ring_config_t worker_ring_configs[RING_COUNT] = {
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

// --- Snoop callback: passively cache cards ---
static void worker_snoop_cb(uint8_t ring_id, const pkt_header_t *hdr,
                            const uint8_t *payload) {
    (void)ring_id;
    if (hdr->type == PKT_CARD_DATA && payload) {
        const pkt_card_data_t *cpkt = (const pkt_card_data_t *)payload;
        if (!card_cache_has(&g_card_cache, cpkt->card_major, cpkt->card_minor)) {
            card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                            cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
        }
    }
}

// --- Send NAK for missing card ---
static void send_nak(uint8_t ring_id, uint8_t dest, uint8_t major, uint8_t minor) {
    pkt_nak_t nak = { .card_major = major, .card_minor = minor, .version = 0 };
    pkt_header_t hdr = {
        .dest = dest,
        .src = g_node_id,
        .type = PKT_NAK,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
        .payload_len = sizeof(pkt_nak_t),
    };
    hdr.crc16 = crc16_ccitt((uint8_t *)&nak, sizeof(nak));
    ring_send(ring_id, &hdr, (uint8_t *)&nak, sizeof(nak));
}

// --- Send result back ---
static void send_result(uint8_t ring_id, uint8_t dest, uint8_t major, uint8_t minor,
                        const uint8_t *data, uint16_t len, uint8_t status) {
    static uint8_t res_pkt[PKT_MAX_PAYLOAD];
    pkt_result_t *res = (pkt_result_t *)res_pkt;
    res->status = status;
    res->card_major = major;
    res->card_minor = minor;
    res->reserved = 0;
    res->data_len = len;
    if (len > 0 && data) memcpy(res->data, data, len);

    uint16_t payload_len = sizeof(pkt_result_t) + len;
    pkt_header_t hdr = {
        .dest = dest,
        .src = g_node_id,
        .type = PKT_RESULT,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = payload_len,
    };
    hdr.crc16 = crc16_ccitt(res_pkt, payload_len);
    ring_send(ring_id, &hdr, res_pkt, payload_len);
}

// --- Handle EXEC packet ---
static void handle_exec(uint8_t ring_id, const pkt_header_t *hdr,
                        const uint8_t *payload) {
    const pkt_exec_t *exec = (const pkt_exec_t *)payload;

    // Check card availability
    if (!card_cache_has(&g_card_cache, exec->card_major, exec->card_minor)) {
        send_nak(ring_id, hdr->src, exec->card_major, exec->card_minor);
        return;
    }

    // Queue for Core 1
    uint8_t next = (g_exec_head + 1) % VM_EXEC_QUEUE_SIZE;
    if (next == g_exec_tail) return;  // Queue full, drop

    // Copy input data to working memory
    uint32_t offset = g_data_write_offset;
    if (exec->data_len > 0 && offset + exec->data_len <= MEM_VM_DATA_SIZE) {
        memcpy(vm_data_mem + offset, exec->data, exec->data_len);
        g_data_write_offset = offset + ((exec->data_len + 3) & ~3u);
    }

    g_exec_queue[g_exec_head] = (exec_request_t){
        .card_major = exec->card_major,
        .card_minor = exec->card_minor,
        .data_len = exec->data_len,
        .src_node = hdr->src,
        .ring_id = ring_id,
        .data_offset = offset,
    };
    __dmb();
    g_exec_head = next;
    multicore_fifo_push_blocking(1);  // Wake Core 1
}

// --- Handle DISCOVER ---
static void handle_discover(uint8_t ring_id, const pkt_header_t *hdr) {
    // Respond with STATUS
    pkt_status_t status = {
        .node_state = 0,  // idle
        .queue_depth = (g_exec_head >= g_exec_tail) ?
            (g_exec_head - g_exec_tail) :
            (VM_EXEC_QUEUE_SIZE - g_exec_tail + g_exec_head),
        .card_count = g_card_cache.entry_count,
        .exec_count = g_vm_ctx.cycles,
        .uptime_sec = time_us_32() / 1000000,
    };
    pkt_header_t resp_hdr = {
        .dest = hdr->src,
        .src = g_node_id,
        .type = PKT_STATUS,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = sizeof(pkt_status_t),
    };
    resp_hdr.crc16 = crc16_ccitt((uint8_t *)&status, sizeof(status));
    ring_send(ring_id, &resp_hdr, (uint8_t *)&status, sizeof(status));
}

// --- Core 0: Ring management ---
static void core0_loop(void) {
    ring_init_all(worker_ring_configs);
    ring_set_snoop_callback(worker_snoop_cb);
    card_cache_warm_from_flash(&g_card_cache);

    uint32_t last_heartbeat = 0;
    uint32_t last_flush = 0;

    while (1) {
        uint32_t now = time_us_32();

        // Poll all rings
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            // CRC check
            uint16_t expected = hdr.crc16;
            if (hdr.payload_len > 0) {
                if (crc16_ccitt(payload, hdr.payload_len) != expected) {
                    ring_rx_done(r);
                    continue;
                }
            }

            bool for_me = (hdr.dest == g_node_id || hdr.dest == ADDR_BROADCAST);

            if (for_me) {
                switch (hdr.type) {
                case PKT_EXEC:
                case PKT_BATCH:
                    handle_exec(r, &hdr, payload);
                    break;
                case PKT_DISCOVER:
                    handle_discover(r, &hdr);
                    break;
                default:
                    break;
                }
            }

            // Forward (broadcast or not-for-me)
            if (!for_me || hdr.dest == ADDR_BROADCAST) {
                uint8_t ttl = PKT_TTL(hdr.flags);
                if (ttl > 0) {
                    ring_forward(r, r);
                }
            }

            ring_rx_done(r);
        }

        // Periodic heartbeat
        if (now - last_heartbeat > HEARTBEAT_INTERVAL_MS * 1000) {
            last_heartbeat = now;
            // Could send unsolicited STATUS here
        }

        // Periodic flash flush (every 10s)
        if (now - last_flush > 10000000) {
            last_flush = now;
            card_cache_flush_to_flash(&g_card_cache);
        }
    }
}

// --- Core 1: VM execution ---
static void core1_entry(void) {
    vm_init(&g_vm_ctx);
    g_vm_ctx.data_base = vm_data_mem;
    g_vm_ctx.data_size = MEM_VM_DATA_SIZE;
    g_vm_ctx.stack_base = vm_stack_mem;
    g_vm_ctx.stack_size = MEM_VM_STACK_SIZE;
    g_vm_ctx.result_buf = result_buf;
    g_vm_ctx.result_capacity = MEM_RESULT_BUF_SIZE;

    while (1) {
        multicore_fifo_pop_blocking();  // Wait for work

        while (g_exec_tail != g_exec_head) {
            exec_request_t req = g_exec_queue[g_exec_tail];
            __dmb();
            g_exec_tail = (g_exec_tail + 1) % VM_EXEC_QUEUE_SIZE;

            // Get card
            uint32_t card_len;
            uint32_t *card = card_cache_get(&g_card_cache,
                                            req.card_major, req.card_minor,
                                            &card_len);
            if (!card) continue;

            // Load and execute
            vm_load_card(&g_vm_ctx, card, card_len, req.card_major, req.card_minor);
            g_vm_ctx.regs[1] = req.data_offset;  // Input data pointer

            vm_execute(&g_vm_ctx, VM_MAX_CYCLES_PER_RUN);

            // Send result
            if (g_vm_ctx.state == VM_HALTED) {
                send_result(req.ring_id, req.src_node,
                            req.card_major, req.card_minor,
                            g_vm_ctx.result_buf, g_vm_ctx.result_len, 0);
            } else {
                send_result(req.ring_id, req.src_node,
                            req.card_major, req.card_minor,
                            NULL, 0, 1);  // Error
            }

            // Reset data write offset after processing
            if (g_exec_tail == g_exec_head) {
                g_data_write_offset = 0;
            }
        }
    }
}

// --- Entry point ---
void role_worker_run(void) {
    card_cache_init(&g_card_cache);

    // Read node ID from flash config
    flash_config_t cfg;
    platform_flash_read(FLASH_CONFIG_BASE - FLASH_FIRMWARE_BASE,
                        (uint8_t *)&cfg, sizeof(cfg));
    if (cfg.magic == FLASH_CONFIG_MAGIC && cfg.node_id != 0xFF) {
        g_node_id = cfg.node_id;
    } else {
        g_node_id = 0xFE;  // Unassigned — will be set by discovery
    }

    multicore_launch_core1(core1_entry);
    core0_loop();  // Never returns
}
