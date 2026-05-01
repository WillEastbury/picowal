#include "roles.h"
#include "../common/config.h"
#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../discovery/discovery.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"

#include <string.h>

// ============================================================
// Worker Role — Pure compute (tree topology)
// GP0: half-duplex link to HEAD (command/result path)
// GP1: half-duplex link to STORAGE (card data path)
// Core 0: Link I/O on both links
// Core 1: VM execution
// ============================================================

// --- Links ---
static link_port_t g_head_link;   // Slave on head link (head is master)
static link_port_t g_store_link;  // Master on storage link (we request cards)

// --- Memory ---
static uint8_t __attribute__((aligned(4))) vm_data_mem[MEM_VM_DATA_SIZE];
static uint8_t __attribute__((aligned(4))) vm_stack_mem[MEM_VM_STACK_SIZE];
static uint8_t __attribute__((aligned(4))) result_buf[MEM_RESULT_BUF_SIZE];

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

// --- Request card from storage node ---
static bool fetch_card_from_storage(uint8_t major, uint8_t minor) {
    pkt_nak_t req = { .card_major = major, .card_minor = minor, .version = 0 };
    pkt_header_t hdr = {
        .dest = ADDR_STORAGE_BASE,
        .src = g_node_id,
        .type = PKT_CARD_REQ,
        .flags = 0,
        .payload_len = sizeof(req),
    };
    hdr.crc16 = crc16_ccitt((uint8_t *)&req, sizeof(req));

    pkt_header_t reply_hdr;
    uint8_t *reply_payload;

    if (link_master_transact(&g_store_link, &hdr, (uint8_t *)&req, sizeof(req),
                             &reply_hdr, &reply_payload, 50000)) {
        if (reply_hdr.type == PKT_CARD_DATA) {
            const pkt_card_data_t *cpkt = (const pkt_card_data_t *)reply_payload;
            card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                             cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
            return true;
        }
    }
    return false;
}

// --- Send result to head ---
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
    link_slave_reply(&g_head_link, &hdr, res_pkt, payload_len);
}

// --- Handle EXEC from head ---
static void handle_exec(const pkt_header_t *hdr, const uint8_t *payload) {
    const pkt_exec_t *exec = (const pkt_exec_t *)payload;

    // Check card cache, fetch from storage if missing
    if (!card_cache_has(&g_card_cache, exec->card_major, exec->card_minor)) {
        if (!fetch_card_from_storage(exec->card_major, exec->card_minor)) {
            // Can't get card — NAK back to head
            send_result(hdr->src, exec->card_major, exec->card_minor, NULL, 0, 2);
            return;
        }
    }

    // Queue for Core 1
    uint8_t next = (g_exec_head + 1) % VM_EXEC_QUEUE_SIZE;
    if (next == g_exec_tail) return;

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

// --- Core 0: Dual-link I/O ---
static void core0_loop(void) {
    // Init head link (slave — head polls us)
    g_head_link = (link_port_t){
        .pin = WORKER_HEAD_PIN,
        .sm = 0,
        .pio_idx = 0,
        .dma_ch = 0,
        .is_master = false,
    };
    link_init_port(&g_head_link);

    // Init storage link (master — we request cards)
    g_store_link = (link_port_t){
        .pin = WORKER_STORE_PIN,
        .sm = 1,
        .pio_idx = 0,
        .dma_ch = 1,
        .is_master = true,
    };
    link_init_port(&g_store_link);

    card_cache_warm_from_flash(&g_card_cache);

    while (1) {
        watchdog_update();

        // Check for commands from head
        pkt_header_t hdr;
        uint8_t *payload;

        if (link_slave_poll_rx(&g_head_link, &hdr, &payload)) {
            switch (hdr.type) {
            case PKT_EXEC:
            case PKT_BATCH:
                handle_exec(&hdr, payload);
                break;
            case PKT_CARD_DATA:
                // Head can also push cards
                {
                    const pkt_card_data_t *cpkt = (const pkt_card_data_t *)payload;
                    card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                                     cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
                }
                break;
            default:
                break;
            }
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

            send_result(req.src_node, req.card_major, req.card_minor,
                        g_vm_ctx.result_buf, g_vm_ctx.result_len,
                        (g_vm_ctx.state == VM_HALTED) ? 0 : 1);

            if (g_exec_tail == g_exec_head) g_data_write_offset = 0;
        }
    }
}

// --- Entry point ---
void role_worker_run(void) {
    card_cache_init(&g_card_cache);
    g_node_id = disc_participant_run(3);  // ROLE_WORKER
    if (g_node_id == 0) g_node_id = 0xFE;

    multicore_launch_core1(core1_entry);
    core0_loop();
}
