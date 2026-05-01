#include "roles.h"
#include "../common/config.h"
#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../discovery/discovery.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"

#include <string.h>

// ============================================================
// Worker Role — Pure compute node (star topology)
// Core 0: Link I/O (single point-to-point to head)
// Core 1: VM execution
// ============================================================
// No relay, no forwarding. Worker just talks to its head.

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
    uint32_t data_offset;
} exec_request_t;

static volatile exec_request_t g_exec_queue[VM_EXEC_QUEUE_SIZE];
static volatile uint8_t g_exec_head = 0;
static volatile uint8_t g_exec_tail = 0;
static volatile uint32_t g_data_write_offset = 0;

// --- Send NAK for missing card ---
static void send_nak(uint8_t dest, uint8_t major, uint8_t minor) {
    pkt_nak_t nak = { .card_major = major, .card_minor = minor, .version = 0 };
    pkt_header_t hdr = {
        .dest = dest,
        .src = g_node_id,
        .type = PKT_NAK,
        .flags = 0,
        .payload_len = sizeof(pkt_nak_t),
    };
    hdr.crc16 = crc16_ccitt((uint8_t *)&nak, sizeof(nak));
    link_worker_send(&hdr, (uint8_t *)&nak, sizeof(nak));
}

// --- Send result back to requester ---
static void send_result(uint8_t dest, uint8_t major, uint8_t minor,
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
        .flags = 0,
        .payload_len = payload_len,
    };
    hdr.crc16 = crc16_ccitt(res_pkt, payload_len);
    link_worker_send(&hdr, res_pkt, payload_len);
}

// --- Handle EXEC packet ---
static void handle_exec(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_exec_t *exec = (const pkt_exec_t *)payload;

    if (!card_cache_has(&g_card_cache, exec->card_major, exec->card_minor)) {
        send_nak(hdr->src, exec->card_major, exec->card_minor);
        return;
    }

    // Queue for Core 1
    uint8_t next = (g_exec_head + 1) % VM_EXEC_QUEUE_SIZE;
    if (next == g_exec_tail) return;  // Full, drop

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
        .data_offset = offset,
    };
    __dmb();
    g_exec_head = next;
    multicore_fifo_push_blocking(1);
}

// --- Handle CARD_DATA (cache it) ---
static void handle_card_data(const uint8_t *payload) {
    const pkt_card_data_t *cpkt = (const pkt_card_data_t *)payload;
    if (!card_cache_has(&g_card_cache, cpkt->card_major, cpkt->card_minor)) {
        card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                         cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
    }
}

// --- Core 0: Link I/O (no forwarding needed!) ---
static void core0_loop(void) {
    link_worker_init();
    card_cache_warm_from_flash(&g_card_cache);

    uint32_t last_heartbeat = 0;
    uint32_t last_flush = 0;

    while (1) {
        watchdog_update();
        uint32_t now = time_us_32();

        // Poll link
        pkt_header_t hdr;
        uint8_t *payload;

        if (link_worker_poll_rx(&hdr, &payload)) {
            switch (hdr.type) {
            case PKT_EXEC:
            case PKT_BATCH:
                handle_exec(&hdr, payload);
                break;
            case PKT_CARD_DATA:
                handle_card_data(payload);
                break;
            default:
                break;
            }
        }

        // Periodic heartbeat status to head
        if (now - last_heartbeat > HEARTBEAT_INTERVAL_MS * 1000) {
            last_heartbeat = now;
            pkt_status_t status = {
                .node_state = (g_exec_tail != g_exec_head) ? 1 : 0,
                .queue_depth = (g_exec_head >= g_exec_tail) ?
                    (g_exec_head - g_exec_tail) :
                    (VM_EXEC_QUEUE_SIZE - g_exec_tail + g_exec_head),
                .card_count = g_card_cache.entry_count,
                .exec_count = g_vm_ctx.cycles,
                .uptime_sec = now / 1000000,
            };
            pkt_header_t shdr = {
                .dest = ADDR_MASTER,
                .src = g_node_id,
                .type = PKT_STATUS,
                .flags = 0,
                .payload_len = sizeof(status),
            };
            shdr.crc16 = crc16_ccitt((uint8_t *)&status, sizeof(status));
            link_worker_send(&shdr, (uint8_t *)&status, sizeof(status));
        }

        // Periodic flash flush
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
        multicore_fifo_pop_blocking();

        while (g_exec_tail != g_exec_head) {
            exec_request_t req = g_exec_queue[g_exec_tail];
            __dmb();
            g_exec_tail = (g_exec_tail + 1) % VM_EXEC_QUEUE_SIZE;

            uint32_t card_len;
            uint32_t *card = card_cache_get(&g_card_cache,
                                            req.card_major, req.card_minor,
                                            &card_len);
            if (!card) continue;

            vm_load_card(&g_vm_ctx, card, card_len, req.card_major, req.card_minor);
            g_vm_ctx.regs[1] = req.data_offset;

            vm_execute(&g_vm_ctx, VM_MAX_CYCLES_PER_RUN);

            if (g_vm_ctx.state == VM_HALTED) {
                send_result(req.src_node, req.card_major, req.card_minor,
                            g_vm_ctx.result_buf, g_vm_ctx.result_len, 0);
            } else {
                send_result(req.src_node, req.card_major, req.card_minor,
                            NULL, 0, 1);
            }

            if (g_exec_tail == g_exec_head) {
                g_data_write_offset = 0;
            }
        }
    }
}

// --- Entry point ---
void role_worker_run(void) {
    card_cache_init(&g_card_cache);

    // Run DHCP-style address discovery
    g_node_id = disc_participant_run(3);  // 3 = ROLE_WORKER
    if (g_node_id == 0) {
        // Fallback: try flash config
        flash_config_t cfg;
        platform_flash_read(FLASH_CONFIG_BASE - FLASH_FIRMWARE_BASE,
                            (uint8_t *)&cfg, sizeof(cfg));
        if (cfg.magic == FLASH_CONFIG_MAGIC && cfg.node_id != 0xFF) {
            g_node_id = cfg.node_id;
        } else {
            g_node_id = 0xFE;
        }
    }

    multicore_launch_core1(core1_entry);
    core0_loop();
}
