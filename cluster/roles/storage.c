#include "roles.h"
#include "../common/config.h"
#include "../common/isa.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../drivers/sd.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"

#include <string.h>

// ============================================================
// Storage Role — SD-backed card server + compute when idle
// Core 0: Ring relay + card serving from SD
// Core 1: VM execution (dual-role compute)
// ============================================================

// --- SD card layout for card storage ---
// Sector 0: Storage header (magic, card count, version)
// Sector 1-N: Card index (major, minor, version, start_sector, sector_count)
// Sector N+1...: Card bytecode data

#define SD_HEADER_SECTOR     0
#define SD_INDEX_START       1
#define SD_INDEX_SECTORS     8        // 8 sectors = 4KB = ~256 card entries
#define SD_DATA_START        (SD_INDEX_START + SD_INDEX_SECTORS)

#define SD_MAGIC             0x50434453  // "PCDS"
#define SD_MAX_CARDS         256

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t card_count;
    uint16_t version;
    uint32_t next_data_sector;   // Next free sector for card data
    uint8_t  reserved[500];      // Pad to 512 bytes
} sd_storage_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  major;
    uint8_t  minor;
    uint16_t version;
    uint32_t start_sector;
    uint32_t byte_len;
    uint32_t reserved;
} sd_card_index_entry_t;  // 16 bytes each, 32 per sector

// --- State ---
static sd_info_t g_sd_info;
static sd_storage_header_t g_sd_header;
static sd_card_index_entry_t g_sd_index[SD_MAX_CARDS];
static card_cache_t g_card_cache;   // SRAM cache
static volatile uint8_t g_node_id = 0xFF;

// VM state (for dual-role compute)
static uint8_t __attribute__((aligned(4))) vm_data_mem[MEM_VM_DATA_SIZE];
static uint8_t __attribute__((aligned(4))) vm_stack_mem[MEM_VM_STACK_SIZE];
static uint8_t __attribute__((aligned(4))) result_buf[MEM_RESULT_BUF_SIZE];
static vm_context_t g_vm_ctx;

typedef struct {
    uint8_t  card_major;
    uint8_t  card_minor;
    uint16_t data_len;
    uint8_t  src_node;
    uint8_t  ring_id;
    uint32_t data_offset;
} exec_request_t;

static volatile exec_request_t g_exec_queue[VM_EXEC_QUEUE_SIZE];
static volatile uint8_t g_exec_head = 0;
static volatile uint8_t g_exec_tail = 0;
static volatile uint32_t g_data_write_offset = 0;

// Ring config
static const ring_config_t storage_ring_configs[RING_COUNT] = {
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

// ============================================================
// SD card management
// ============================================================

static bool sd_storage_init(void) {
    if (!sd_init(&g_sd_info)) return false;

    // Read header
    uint8_t sector_buf[512];
    if (!sd_read_sector(SD_HEADER_SECTOR, sector_buf)) return false;

    memcpy(&g_sd_header, sector_buf, sizeof(g_sd_header));

    if (g_sd_header.magic != SD_MAGIC) {
        // Fresh card — format
        memset(&g_sd_header, 0, sizeof(g_sd_header));
        g_sd_header.magic = SD_MAGIC;
        g_sd_header.card_count = 0;
        g_sd_header.version = 1;
        g_sd_header.next_data_sector = SD_DATA_START;

        memset(sector_buf, 0, 512);
        memcpy(sector_buf, &g_sd_header, sizeof(g_sd_header));
        sd_write_sector(SD_HEADER_SECTOR, sector_buf);
        return true;
    }

    // Load index
    uint16_t count = g_sd_header.card_count;
    if (count > SD_MAX_CARDS) count = SD_MAX_CARDS;

    for (uint32_t s = 0; s < SD_INDEX_SECTORS && count > 0; s++) {
        if (!sd_read_sector(SD_INDEX_START + s, sector_buf)) break;
        uint32_t entries_in_sector = 512 / sizeof(sd_card_index_entry_t);  // 32
        for (uint32_t e = 0; e < entries_in_sector && count > 0; e++) {
            uint32_t idx = s * entries_in_sector + e;
            memcpy(&g_sd_index[idx], sector_buf + e * sizeof(sd_card_index_entry_t),
                   sizeof(sd_card_index_entry_t));
            count--;
        }
    }

    return true;
}

// Find card on SD by major/minor
static int sd_find_card(uint8_t major, uint8_t minor) {
    for (uint16_t i = 0; i < g_sd_header.card_count; i++) {
        if (g_sd_index[i].major == major && g_sd_index[i].minor == minor) {
            return (int)i;
        }
    }
    return -1;
}

// Load card from SD into buffer
static bool sd_load_card(uint8_t major, uint8_t minor, uint8_t *buf, uint32_t *len) {
    int idx = sd_find_card(major, minor);
    if (idx < 0) return false;

    sd_card_index_entry_t *entry = &g_sd_index[idx];
    uint32_t sectors_needed = (entry->byte_len + 511) / 512;

    if (!sd_read_sectors(entry->start_sector, sectors_needed, buf)) return false;
    *len = entry->byte_len;
    return true;
}

// Store card to SD
static bool sd_store_card(uint8_t major, uint8_t minor, uint16_t version,
                          const uint8_t *bytecode, uint32_t len) {
    // Check if already exists
    int idx = sd_find_card(major, minor);
    if (idx >= 0 && g_sd_index[idx].version >= version) {
        return true;  // Already have same or newer
    }

    uint32_t sectors_needed = (len + 511) / 512;
    uint32_t start = g_sd_header.next_data_sector;

    // Write bytecode
    // Pad last sector
    static uint8_t pad_buf[512];
    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint32_t offset = s * 512;
        uint32_t chunk = (len - offset > 512) ? 512 : (len - offset);
        if (chunk == 512) {
            sd_write_sector(start + s, bytecode + offset);
        } else {
            memset(pad_buf, 0xFF, 512);
            memcpy(pad_buf, bytecode + offset, chunk);
            sd_write_sector(start + s, pad_buf);
        }
    }

    // Update index
    if (idx < 0) {
        idx = g_sd_header.card_count++;
    }
    g_sd_index[idx] = (sd_card_index_entry_t){
        .major = major, .minor = minor, .version = version,
        .start_sector = start, .byte_len = len,
    };

    g_sd_header.next_data_sector = start + sectors_needed;

    // Write index sector
    uint32_t idx_sector = SD_INDEX_START + (idx / 32);
    uint8_t sector_buf[512];
    sd_read_sector(idx_sector, sector_buf);
    memcpy(sector_buf + (idx % 32) * sizeof(sd_card_index_entry_t),
           &g_sd_index[idx], sizeof(sd_card_index_entry_t));
    sd_write_sector(idx_sector, sector_buf);

    // Write header
    memset(sector_buf, 0, 512);
    memcpy(sector_buf, &g_sd_header, sizeof(g_sd_header));
    sd_write_sector(SD_HEADER_SECTOR, sector_buf);

    return true;
}

// ============================================================
// Ring handlers
// ============================================================

// Snoop: cache cards + store to SD
static void storage_snoop_cb(uint8_t ring_id, const pkt_header_t *hdr,
                             const uint8_t *payload) {
    (void)ring_id;
    if (hdr->type == PKT_CARD_DATA && payload) {
        const pkt_card_data_t *cpkt = (const pkt_card_data_t *)payload;

        // Cache in SRAM
        if (!card_cache_has(&g_card_cache, cpkt->card_major, cpkt->card_minor)) {
            card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                            cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
        }

        // Persist to SD
        sd_store_card(cpkt->card_major, cpkt->card_minor,
                      cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
    }
}

// Handle CARD_REQ: someone needs a card we might have on SD
static void handle_card_request(uint8_t ring_id, const pkt_header_t *hdr,
                                const uint8_t *payload) {
    const pkt_nak_t *req = (const pkt_nak_t *)payload;

    // Try SRAM cache first
    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache, req->card_major, req->card_minor,
                                    &card_len);

    static uint8_t sd_buf[CARD_MAX_SIZE];
    if (!card) {
        // Try SD
        uint32_t byte_len;
        if (!sd_load_card(req->card_major, req->card_minor, sd_buf, &byte_len)) {
            return;  // We don't have it either
        }
        // Cache it
        card_cache_store(&g_card_cache, req->card_major, req->card_minor,
                         0, sd_buf, byte_len);
        card = card_cache_get(&g_card_cache, req->card_major, req->card_minor, &card_len);
        if (!card) return;
    }

    // Send card on ring
    static uint8_t card_pkt_buf[PKT_MAX_PAYLOAD];
    pkt_card_data_t *cpkt = (pkt_card_data_t *)card_pkt_buf;
    cpkt->card_major = req->card_major;
    cpkt->card_minor = req->card_minor;
    cpkt->version = 1;
    cpkt->bytecode_len = card_len * 4;
    memcpy(cpkt->bytecode, card, card_len * 4);

    uint16_t plen = sizeof(pkt_card_data_t) + card_len * 4;
    pkt_header_t resp = {
        .dest = ADDR_BROADCAST,
        .src = g_node_id,
        .type = PKT_CARD_DATA,
        .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0),
        .payload_len = plen,
    };
    resp.crc16 = crc16_ccitt(card_pkt_buf, plen);
    ring_send(ring_id, &resp, card_pkt_buf, plen);
}

// Handle EXEC (storage nodes also compute)
static void handle_exec(uint8_t ring_id, const pkt_header_t *hdr,
                        const uint8_t *payload) {
    const pkt_exec_t *exec = (const pkt_exec_t *)payload;

    if (!card_cache_has(&g_card_cache, exec->card_major, exec->card_minor)) {
        // Try loading from SD
        static uint8_t sd_buf[CARD_MAX_SIZE];
        uint32_t byte_len;
        if (sd_load_card(exec->card_major, exec->card_minor, sd_buf, &byte_len)) {
            card_cache_store(&g_card_cache, exec->card_major, exec->card_minor,
                             0, sd_buf, byte_len);
        } else {
            // NAK
            pkt_nak_t nak = { .card_major = exec->card_major,
                              .card_minor = exec->card_minor, .version = 0 };
            pkt_header_t nak_hdr = {
                .dest = hdr->src, .src = g_node_id, .type = PKT_NAK,
                .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 1),
                .payload_len = sizeof(nak),
            };
            nak_hdr.crc16 = crc16_ccitt((uint8_t *)&nak, sizeof(nak));
            ring_send(ring_id, &nak_hdr, (uint8_t *)&nak, sizeof(nak));
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
        .card_major = exec->card_major, .card_minor = exec->card_minor,
        .data_len = exec->data_len, .src_node = hdr->src,
        .ring_id = ring_id, .data_offset = offset,
    };
    __dmb();
    g_exec_head = next;
    multicore_fifo_push_blocking(1);
}

// --- Core 0: Ring + SD serving ---
static void core0_loop(void) {
    ring_init_all(storage_ring_configs);
    ring_set_snoop_callback(storage_snoop_cb);
    card_cache_warm_from_flash(&g_card_cache);

    while (1) {
        for (uint8_t r = 0; r < RING_COUNT; r++) {
            pkt_header_t hdr;
            uint8_t *payload;
            if (!ring_poll_rx(r, &hdr, &payload)) continue;

            uint16_t expected = hdr.crc16;
            if (hdr.payload_len > 0 &&
                crc16_ccitt(payload, hdr.payload_len) != expected) {
                ring_rx_done(r);
                continue;
            }

            bool for_me = (hdr.dest == g_node_id || hdr.dest == ADDR_BROADCAST);

            if (for_me) {
                switch (hdr.type) {
                case PKT_EXEC:
                case PKT_BATCH:
                    handle_exec(r, &hdr, payload);
                    break;
                case PKT_NAK:
                case PKT_CARD_REQ:
                    handle_card_request(r, &hdr, payload);
                    break;
                default:
                    break;
                }
            }

            if (!for_me || hdr.dest == ADDR_BROADCAST) {
                uint8_t ttl = PKT_TTL(hdr.flags);
                if (ttl > 0) ring_forward(r, r);
            }

            ring_rx_done(r);
        }
    }
}

// --- Core 1: VM execution (same as worker) ---
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
                                            req.card_major, req.card_minor, &card_len);
            if (!card) continue;

            vm_load_card(&g_vm_ctx, card, card_len, req.card_major, req.card_minor);
            g_vm_ctx.regs[1] = req.data_offset;
            vm_execute(&g_vm_ctx, VM_MAX_CYCLES_PER_RUN);

            // Send result
            static uint8_t res_pkt[PKT_MAX_PAYLOAD];
            pkt_result_t *res = (pkt_result_t *)res_pkt;
            res->status = (g_vm_ctx.state == VM_HALTED) ? 0 : 1;
            res->card_major = req.card_major;
            res->card_minor = req.card_minor;
            res->data_len = g_vm_ctx.result_len;
            if (g_vm_ctx.result_len > 0) {
                memcpy(res->data, g_vm_ctx.result_buf, g_vm_ctx.result_len);
            }

            uint16_t plen = sizeof(pkt_result_t) + g_vm_ctx.result_len;
            pkt_header_t hdr = {
                .dest = req.src_node, .src = g_node_id, .type = PKT_RESULT,
                .flags = PKT_MAKE_FLAGS(DEFAULT_TTL, 0), .payload_len = plen,
            };
            hdr.crc16 = crc16_ccitt(res_pkt, plen);
            ring_send(req.ring_id, &hdr, res_pkt, plen);

            if (g_exec_tail == g_exec_head) g_data_write_offset = 0;
        }
    }
}

// --- Entry point ---
void role_storage_run(void) {
    card_cache_init(&g_card_cache);

    // Init SD storage
    if (!sd_storage_init()) {
        // SD failed — fall back to worker role
        role_worker_run();
        return;
    }

    // Read node ID
    flash_config_t cfg;
    platform_flash_read(FLASH_CONFIG_BASE - FLASH_FIRMWARE_BASE,
                        (uint8_t *)&cfg, sizeof(cfg));
    g_node_id = (cfg.magic == FLASH_CONFIG_MAGIC && cfg.node_id != 0xFF) ?
                cfg.node_id : 0xFE;

    multicore_launch_core1(core1_entry);
    core0_loop();
}
