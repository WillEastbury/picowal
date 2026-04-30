// ============================================================
// PicoCluster Worker Node — Main firmware for RP2350
// ============================================================
// Core 0: Ring relay + bus snooping (PIO/DMA management)
// Core 1: VM execution (pure compute)
// ============================================================

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"

// ============================================================
// Configuration
// ============================================================

#define NODE_ID_UNASSIGNED  0xFE
#define OVERCLOCK_KHZ       450000
#define VM_MAX_CYCLES       100000  // Max cycles per execution before yield

// Memory regions
static uint8_t __attribute__((aligned(4))) vm_data_mem[128 * 1024];   // 128KB working memory
static uint8_t __attribute__((aligned(4))) vm_stack_mem[32 * 1024];   // 32KB stack
static uint8_t __attribute__((aligned(4))) result_buf[16 * 1024];     // 16KB result buffer

// Shared state between cores
static volatile uint8_t g_node_id = NODE_ID_UNASSIGNED;
static card_cache_t g_card_cache;
static vm_context_t g_vm_ctx;

// Inter-core command queue
typedef struct {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t data_len;
    uint8_t  src_node;
    uint8_t  ring_id;       // Which ring to send result on
    uint8_t *data_ptr;      // Pointer into data_mem where input was copied
} exec_request_t;

static volatile exec_request_t g_exec_queue[8];
static volatile uint8_t g_exec_head = 0;
static volatile uint8_t g_exec_tail = 0;

// ============================================================
// Ring pin assignments (4 rings × 2 pins)
// ============================================================

static const ring_config_t ring_configs[RING_COUNT] = {
    // Express 1
    { .pio_block = 0, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = 0, .pin_rx = 1,
      .dma_ch_tx = 0, .dma_ch_rx = 1,
      .baud_rate = 20000000 },
    // Express 2
    { .pio_block = 0, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = 2, .pin_rx = 3,
      .dma_ch_tx = 2, .dma_ch_rx = 3,
      .baud_rate = 20000000 },
    // Normal
    { .pio_block = 1, .sm_tx = 0, .sm_rx = 1,
      .pin_tx = 4, .pin_rx = 5,
      .dma_ch_tx = 4, .dma_ch_rx = 5,
      .baud_rate = 20000000 },
    // Storage
    { .pio_block = 1, .sm_tx = 2, .sm_rx = 3,
      .pin_tx = 6, .pin_rx = 7,
      .dma_ch_tx = 6, .dma_ch_rx = 7,
      .baud_rate = 20000000 },
};

// ============================================================
// Bus snooping callback — cache cards passively
// ============================================================

static void snoop_callback(uint8_t ring_id, const pkt_header_t *hdr,
                           const uint8_t *payload) {
    if (hdr->type == PKT_CARD_DATA && payload) {
        const pkt_card_data_t *card_pkt = (const pkt_card_data_t *)payload;
        // Cache if we don't already have this version
        if (!card_cache_has(&g_card_cache, card_pkt->card_major, card_pkt->card_minor)) {
            card_cache_store(&g_card_cache,
                            card_pkt->card_major,
                            card_pkt->card_minor,
                            card_pkt->version,
                            card_pkt->bytecode,
                            card_pkt->bytecode_len);
        }
    }
}

// ============================================================
// Core 0: Ring relay + dispatch
// ============================================================

static void handle_exec_packet(uint8_t ring_id, const pkt_header_t *hdr,
                               const uint8_t *payload) {
    const pkt_exec_t *exec = (const pkt_exec_t *)payload;

    // Check if we have the card
    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache,
                                    exec->card_major, exec->card_minor,
                                    &card_len);
    if (!card) {
        // NAK — we don't have this card
        pkt_header_t nak_hdr = {
            .dest = hdr->src,
            .src = g_node_id,
            .type = PKT_NAK,
            .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
            .payload_len = sizeof(pkt_nak_t),
        };
        pkt_nak_t nak = {
            .card_major = exec->card_major,
            .card_minor = exec->card_minor,
            .version = 0,
        };
        nak_hdr.crc16 = crc16_ccitt((uint8_t *)&nak, sizeof(nak));
        ring_send(ring_id, &nak_hdr, (uint8_t *)&nak, sizeof(nak));
        return;
    }

    // Queue execution for Core 1
    uint8_t next_head = (g_exec_head + 1) & 7;
    if (next_head == g_exec_tail) {
        // Queue full — drop (or could NAK with busy)
        return;
    }

    // Copy input data into working memory
    uint32_t data_offset = 0; // Simple bump allocator per request
    if (exec->data_len > 0 && exec->data_len <= sizeof(vm_data_mem) / 2) {
        memcpy(vm_data_mem + data_offset, exec->data, exec->data_len);
    }

    g_exec_queue[g_exec_head] = (exec_request_t){
        .card_major = exec->card_major,
        .card_minor = exec->card_minor,
        .data_len = exec->data_len,
        .src_node = hdr->src,
        .ring_id = ring_id,
        .data_ptr = vm_data_mem + data_offset,
    };
    __dmb();
    g_exec_head = next_head;

    // Signal Core 1
    multicore_fifo_push_blocking(1);
}

static void core0_main_loop(void) {
    ring_init_all(ring_configs);
    ring_set_snoop_callback(snoop_callback);

    // Warm cache from flash
    card_cache_warm_from_flash(&g_card_cache);

    // Send READY status
    // TODO: discovery protocol

    while (1) {
        // Poll all rings
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;

            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            // Verify CRC
            uint16_t expected_crc = hdr.crc16;
            hdr.crc16 = 0;
            uint16_t calc_crc = crc16_ccitt(payload, hdr.payload_len);
            if (calc_crc != expected_crc) {
                ring_rx_done(r);
                continue;  // Drop corrupted packet
            }

            // Is it for us?
            if (hdr.dest == g_node_id || hdr.dest == ADDR_BROADCAST) {
                switch (hdr.type) {
                case PKT_EXEC:
                case PKT_BATCH:
                    handle_exec_packet(r, &hdr, payload);
                    break;
                case PKT_CARD_DATA:
                    // Snoop callback already handled caching
                    break;
                case PKT_DISCOVER:
                    // TODO: respond with our ID + neighbor info
                    break;
                default:
                    break;
                }

                // Broadcast packets still get forwarded
                if (hdr.dest == ADDR_BROADCAST) {
                    uint8_t ttl = PKT_TTL(hdr.flags);
                    if (ttl > 0) {
                        hdr.flags = PKT_MAKE_FLAGS(ttl - 1, PKT_PRIORITY(hdr.flags));
                        ring_forward(r, r);  // Continue on same ring
                    }
                }
            } else {
                // Not for us — forward (TTL decrement)
                uint8_t ttl = PKT_TTL(hdr.flags);
                if (ttl > 0) {
                    hdr.flags = PKT_MAKE_FLAGS(ttl - 1, PKT_PRIORITY(hdr.flags));
                    ring_forward(r, r);
                }
            }

            ring_rx_done(r);
        }

        // Check if Core 1 has results to send back
        if (multicore_fifo_rvalid()) {
            uint32_t signal = multicore_fifo_pop_blocking();
            if (signal == 0xDONE) {
                // Read result from shared buffer and send
                // TODO: build result packet from g_vm_ctx.result_buf
            }
        }

        // Idle: flush dirty cards to flash
        card_cache_flush_to_flash(&g_card_cache);
    }
}

// ============================================================
// Core 1: VM execution
// ============================================================

static void core1_entry(void) {
    vm_init(&g_vm_ctx);
    g_vm_ctx.data_base = vm_data_mem;
    g_vm_ctx.data_size = sizeof(vm_data_mem);
    g_vm_ctx.stack_base = vm_stack_mem;
    g_vm_ctx.stack_size = sizeof(vm_stack_mem);
    g_vm_ctx.result_buf = result_buf;
    g_vm_ctx.result_capacity = sizeof(result_buf);

    while (1) {
        // Wait for work from Core 0
        multicore_fifo_pop_blocking();

        // Drain exec queue
        while (g_exec_tail != g_exec_head) {
            exec_request_t req = g_exec_queue[g_exec_tail];
            __dmb();
            g_exec_tail = (g_exec_tail + 1) & 7;

            // Look up card
            uint32_t card_len;
            uint32_t *card = card_cache_get(&g_card_cache,
                                            req.card_major, req.card_minor,
                                            &card_len);
            if (!card) continue;  // Should not happen (checked on Core 0)

            // Load card into VM
            vm_load_card(&g_vm_ctx, card, card_len,
                         req.card_major, req.card_minor);

            // Set up input data pointer in R1
            g_vm_ctx.regs[1] = (uint32_t)(req.data_ptr - vm_data_mem);

            // Execute
            vm_execute(&g_vm_ctx, VM_MAX_CYCLES);

            // Build result packet and signal Core 0
            if (g_vm_ctx.state == VM_HALTED) {
                // Result is in result_buf, length in result_len
                // Signal Core 0 to send it back
                multicore_fifo_push_blocking(0xD09E);  // DONE signal
            }
        }
    }
}

// ============================================================
// Entry point
// ============================================================

int main(void) {
    // Overclock to 450 MHz
    set_sys_clock_khz(OVERCLOCK_KHZ, true);

    // Init stdio for debug
    stdio_init_all();

    // Init card cache
    card_cache_init(&g_card_cache);

    // Assign node ID (from flash or discovery)
    // TODO: proper discovery protocol
    g_node_id = NODE_ID_UNASSIGNED;

    // Launch Core 1 (VM executor)
    multicore_launch_core1(core1_entry);

    // Core 0: ring relay + dispatch
    core0_main_loop();

    return 0;  // Never reached
}

// ============================================================
// Platform stubs (implemented by ring driver)
// ============================================================

bool pio_ring_recv(uint8_t ring_id, uint8_t *buf, uint32_t *len) {
    pkt_header_t hdr;
    uint8_t *payload;
    if (ring_poll_rx(ring_id, &hdr, &payload)) {
        *len = hdr.payload_len;
        memcpy(buf, payload, hdr.payload_len);
        ring_rx_done(ring_id);
        return true;
    }
    return false;
}

bool pio_ring_peek(uint8_t ring_id, uint8_t *buf, uint32_t *len) {
    // Non-blocking variant
    return pio_ring_recv(ring_id, buf, len);
}

void pio_ring_send(uint8_t ring_id, const uint8_t *buf, uint32_t len) {
    pkt_header_t hdr = {
        .dest = ADDR_MASTER,
        .src = g_node_id,
        .type = PKT_RESULT,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = (uint16_t)len,
    };
    hdr.crc16 = crc16_ccitt(buf, len);
    ring_send(ring_id, &hdr, buf, len);
}

void pio_ring_flush(uint8_t ring_id) {
    (void)ring_id;
    // DMA handles flushing automatically
}

void dma_start(uint8_t channel, void *dest, const void *src, uint32_t len) {
    dma_channel_config c = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure(channel, &c, dest, src, len, true);
}

bool dma_busy(uint8_t channel) {
    return dma_channel_is_busy(channel);
}

uint32_t get_cycle_count(void) {
    return time_us_32();  // Use timer as proxy
}

uint8_t get_node_id(void) {
    return g_node_id;
}
