#include "roles.h"
#include "../common/config.h"
#include "../common/packet.h"
#include "../common/card_cache.h"
#include "../common/ring.h"
#include "../drivers/sd.h"
#include "../discovery/discovery.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"

#include <string.h>

// ============================================================
// Storage Role — Card server with cross-connects
// GP0-GP3: slave ports (workers request cards from us)
// GP4-GP5: master cross-connect ports (replicate/fetch from peers)
// SPI1: SD card (persistent card storage)
// Core 0: Serve card requests from workers + cross-connect I/O
// Core 1: SD card reads (background prefetch)
// ============================================================

// --- Links ---
static link_port_t g_worker_ports[STORE_WORKER_COUNT];  // Slave (workers pull from us)
static link_port_t g_xconn_ports[STORE_XCONN_COUNT];   // Master (we pull/push to peers)

// --- State ---
static card_cache_t g_card_cache;
static volatile uint8_t g_node_id = 0xFF;

// --- SD card index (maps card_major:minor → sector on SD) ---
#define SD_INDEX_MAX 1024
typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint16_t version;
    uint32_t sector;    // Starting sector on SD
    uint32_t length;    // Bytecode length in bytes
} sd_card_entry_t;

static sd_card_entry_t g_sd_index[SD_INDEX_MAX];
static uint16_t g_sd_index_count = 0;

// --- Load SD index from first sectors ---
static void sd_load_index(void) {
    // Index stored at sector 0-7 (4KB)
    uint8_t buf[512];
    g_sd_index_count = 0;

    for (uint32_t s = 0; s < 8 && g_sd_index_count < SD_INDEX_MAX; s++) {
        if (!sd_read_sector(s, buf)) break;

        // Each entry is 12 bytes: [major:1][minor:1][version:2][sector:4][length:4]
        for (uint32_t off = 0; off < 512 && g_sd_index_count < SD_INDEX_MAX; off += 12) {
            sd_card_entry_t *e = &g_sd_index[g_sd_index_count];
            e->major = buf[off];
            e->minor = buf[off + 1];
            if (e->major == 0xFF) return;  // End marker
            memcpy(&e->version, &buf[off + 2], 2);
            memcpy(&e->sector, &buf[off + 4], 4);
            memcpy(&e->length, &buf[off + 8], 4);
            g_sd_index_count++;
        }
    }
}

// --- Find card in SD index ---
static sd_card_entry_t *sd_find_card(uint8_t major, uint8_t minor) {
    for (uint16_t i = 0; i < g_sd_index_count; i++) {
        if (g_sd_index[i].major == major && g_sd_index[i].minor == minor) {
            return &g_sd_index[i];
        }
    }
    return NULL;
}

// --- Load card from SD into cache ---
static bool sd_load_card_to_cache(uint8_t major, uint8_t minor) {
    sd_card_entry_t *entry = sd_find_card(major, minor);
    if (!entry) return false;

    // Read sectors into temp buffer
    static uint8_t card_buf[PKT_MAX_PAYLOAD];
    uint32_t remaining = entry->length;
    uint32_t offset = 0;
    uint32_t sector = entry->sector;

    while (remaining > 0 && offset < sizeof(card_buf)) {
        uint8_t sec_buf[512];
        if (!sd_read_sector(sector++, sec_buf)) return false;
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        memcpy(card_buf + offset, sec_buf, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    card_cache_store(&g_card_cache, major, minor, entry->version,
                     card_buf, entry->length);
    return true;
}

// --- Try to fetch card from peer storage via cross-connect ---
static bool xconn_fetch_card(uint8_t major, uint8_t minor) {
    pkt_nak_t req = { .card_major = major, .card_minor = minor, .version = 0 };
    pkt_header_t hdr = {
        .dest = ADDR_BROADCAST,
        .src = g_node_id,
        .type = PKT_CARD_REQ,
        .flags = 0,
        .payload_len = sizeof(req),
    };
    hdr.crc16 = crc16_ccitt((uint8_t *)&req, sizeof(req));

    // Try each cross-connect peer
    for (uint8_t x = 0; x < STORE_XCONN_COUNT; x++) {
        pkt_header_t reply_hdr;
        uint8_t *reply_payload;

        if (link_master_transact(&g_xconn_ports[x], &hdr, (uint8_t *)&req, sizeof(req),
                                 &reply_hdr, &reply_payload, 30000)) {
            if (reply_hdr.type == PKT_CARD_DATA) {
                const pkt_card_data_t *cpkt = (const pkt_card_data_t *)reply_payload;
                card_cache_store(&g_card_cache, cpkt->card_major, cpkt->card_minor,
                                 cpkt->version, cpkt->bytecode, cpkt->bytecode_len);
                return true;
            }
        }
    }
    return false;
}

// --- Handle card request from a worker ---
static void handle_card_request(link_port_t *port, const pkt_header_t *hdr,
                                const uint8_t *payload) {
    const pkt_nak_t *req = (const pkt_nak_t *)payload;
    uint8_t major = req->card_major;
    uint8_t minor = req->card_minor;

    // Try SRAM cache first
    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache, major, minor, &card_len);

    // Cache miss — try SD
    if (!card) {
        sd_load_card_to_cache(major, minor);
        card = card_cache_get(&g_card_cache, major, minor, &card_len);
    }

    // Still miss — try cross-connect peers
    if (!card) {
        xconn_fetch_card(major, minor);
        card = card_cache_get(&g_card_cache, major, minor, &card_len);
    }

    if (card) {
        // Reply with CARD_DATA
        static uint8_t reply_buf[PKT_MAX_PAYLOAD];
        pkt_card_data_t *cpkt = (pkt_card_data_t *)reply_buf;
        cpkt->card_major = major;
        cpkt->card_minor = minor;
        cpkt->version = 1;
        cpkt->bytecode_len = card_len;
        memcpy(cpkt->bytecode, card, card_len);

        uint16_t plen = sizeof(pkt_card_data_t) + card_len;
        pkt_header_t reply_hdr = {
            .dest = hdr->src,
            .src = g_node_id,
            .type = PKT_CARD_DATA,
            .flags = 0,
            .payload_len = plen,
        };
        reply_hdr.crc16 = crc16_ccitt(reply_buf, plen);
        link_slave_reply(port, &reply_hdr, reply_buf, plen);
    } else {
        // NAK — card not found anywhere
        pkt_nak_t nak = { .card_major = major, .card_minor = minor, .version = 0 };
        pkt_header_t nak_hdr = {
            .dest = hdr->src,
            .src = g_node_id,
            .type = PKT_NAK,
            .flags = 0,
            .payload_len = sizeof(nak),
        };
        nak_hdr.crc16 = crc16_ccitt((uint8_t *)&nak, sizeof(nak));
        link_slave_reply(port, &nak_hdr, (uint8_t *)&nak, sizeof(nak));
    }
}

// --- Handle cross-connect request from peer storage ---
static void handle_xconn_request(link_port_t *port, const pkt_header_t *hdr,
                                 const uint8_t *payload) {
    // Same as worker request but don't recurse to cross-connect
    const pkt_nak_t *req = (const pkt_nak_t *)payload;

    uint32_t card_len;
    uint32_t *card = card_cache_get(&g_card_cache, req->card_major, req->card_minor, &card_len);

    if (!card) {
        sd_load_card_to_cache(req->card_major, req->card_minor);
        card = card_cache_get(&g_card_cache, req->card_major, req->card_minor, &card_len);
    }

    if (card) {
        static uint8_t reply_buf[PKT_MAX_PAYLOAD];
        pkt_card_data_t *cpkt = (pkt_card_data_t *)reply_buf;
        cpkt->card_major = req->card_major;
        cpkt->card_minor = req->card_minor;
        cpkt->version = 1;
        cpkt->bytecode_len = card_len;
        memcpy(cpkt->bytecode, card, card_len);

        uint16_t plen = sizeof(pkt_card_data_t) + card_len;
        pkt_header_t reply_hdr = {
            .dest = hdr->src,
            .src = g_node_id,
            .type = PKT_CARD_DATA,
            .flags = 0,
            .payload_len = plen,
        };
        reply_hdr.crc16 = crc16_ccitt(reply_buf, plen);
        link_slave_reply(port, &reply_hdr, reply_buf, plen);
    }
}

// --- Core 0: Service worker requests + cross-connect ---
static void core0_loop(void) {
    // Init worker ports (slave — workers are masters requesting cards)
    link_init_ports(g_worker_ports, STORE_WORKER_COUNT,
                    STORE_WORKER_BASE, 0, false);

    // Init cross-connect ports (master — we request from peers)
    // Use PIO1 for cross-connects
    link_init_ports(g_xconn_ports, STORE_XCONN_COUNT,
                    STORE_XCONN_BASE, 1, false);  // Slave: peers request from us too

    card_cache_warm_from_flash(&g_card_cache);

    while (1) {
        watchdog_update();

        // Poll worker ports
        for (uint8_t w = 0; w < STORE_WORKER_COUNT; w++) {
            pkt_header_t hdr;
            uint8_t *payload;
            if (link_slave_poll_rx(&g_worker_ports[w], &hdr, &payload)) {
                if (hdr.type == PKT_CARD_REQ) {
                    handle_card_request(&g_worker_ports[w], &hdr, payload);
                }
            }
        }

        // Poll cross-connect ports (peer requests)
        for (uint8_t x = 0; x < STORE_XCONN_COUNT; x++) {
            pkt_header_t hdr;
            uint8_t *payload;
            if (link_slave_poll_rx(&g_xconn_ports[x], &hdr, &payload)) {
                if (hdr.type == PKT_CARD_REQ) {
                    handle_xconn_request(&g_xconn_ports[x], &hdr, payload);
                }
            }
        }
    }
}

// --- Core 1: Background SD prefetch ---
static void core1_prefetch(void) {
    // Prefetch popular cards into SRAM cache at startup
    for (uint16_t i = 0; i < g_sd_index_count && i < 32; i++) {
        sd_load_card_to_cache(g_sd_index[i].major, g_sd_index[i].minor);
    }

    // Then idle — wake on demand via multicore FIFO if needed
    while (1) {
        multicore_fifo_pop_blocking();
        // Could handle async card load requests here
    }
}

// --- Entry point ---
void role_storage_run(void) {
    card_cache_init(&g_card_cache);
    sd_init();
    sd_load_index();

    g_node_id = disc_participant_run(2);  // ROLE_STORAGE
    if (g_node_id == 0) g_node_id = ADDR_STORAGE_BASE;

    multicore_launch_core1(core1_prefetch);
    core0_loop();
}
