#include "net_core.h"
#include "wal_defs.h"
#include "wal_fence.h"
#include "wal_dma.h"
#include "kv_flash.h"
#include "kv_sd.h"
#include "ili9488.h"
#include "httpd/web_server.h"
#include "udp_wal.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/dhcp.h"

#include <string.h>
#include <stdio.h>

// ============================================================
// Request ID allocator (256 IDs, bitmap)
// ============================================================

static uint32_t req_id_bitmap[8]; // 256 bits

static int alloc_req_id(void) {
    for (int w = 0; w < 8; w++) {
        if (req_id_bitmap[w] != 0xFFFFFFFF) {
            for (int b = 0; b < 32; b++) {
                if (!(req_id_bitmap[w] & (1u << b))) {
                    req_id_bitmap[w] |= (1u << b);
                    return w * 32 + b;
                }
            }
        }
    }
    return -1;
}

static void free_req_id(uint8_t id) {
    req_id_bitmap[id / 32] &= ~(1u << (id % 32));
}

// ============================================================
// TCP state — zero-copy design
//
// Instead of rx_buf, we parse the frame header (7 bytes) from
// pbufs inline and pass value data pointers directly to Core 1.
// For contiguous pbuf payloads, Core 1 reads from pbuf memory
// (true zero-copy). For fragmented payloads, we DMA to a slot.
// ============================================================

// ============================================================
// Connection state: auth + data phases
// ============================================================

typedef enum {
    CONN_READY         // accepting WAL ops (no auth required)
} conn_state_t;

typedef enum {
    RX_OPCODE,         // waiting for 1-byte opcode
    RX_APPEND_HDR,     // accumulating 7-byte append header
    RX_APPEND_DATA,    // streaming value data (zero-copy or to slot)
    RX_READ_KEY,       // accumulating 4-byte key hash
} rx_phase_t;

typedef struct {
    wal_state_t    *wal;
    struct tcp_pcb *pcb;
    struct tcp_pcb *listen_pcb;  // listener
    bool            connected;
    conn_state_t    conn_state;

    // Frame parser state
    rx_phase_t phase;
    uint8_t    hdr_buf[32];  // accumulator (max 32 bytes for auth response)
    uint8_t    hdr_pos;

    // Current APPEND in progress
    uint32_t   cur_key_hash;
    uint16_t   cur_value_len;
    uint8_t    cur_delta_op;
    int        cur_slot;
    uint16_t   cur_written;
} net_ctx_t;

static net_ctx_t g_ctx;

// Forward declarations for TCP callbacks
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void on_err(void *arg, err_t err);

// ============================================================
// Find a free buffer slot
// ============================================================

static int alloc_slot(wal_state_t *wal) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (__atomic_exchange_n(&wal->slot_free[i], 0, __ATOMIC_ACQ_REL)) return i;
    }
    return -1;
}

// ============================================================
// TCP send helpers
// ============================================================

static void tcp_send_bytes(struct tcp_pcb *pcb, const uint8_t *data, uint16_t len) {
    if (!pcb) return;
    tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
}

// Flush accumulated tcp_write data — call after building a complete response
static void tcp_flush(struct tcp_pcb *pcb) {
    if (pcb) tcp_output(pcb);
}

static void send_error(struct tcp_pcb *pcb, uint8_t code) {
    uint8_t resp[2] = {WIRE_ERROR, code};
    tcp_send_bytes(pcb, resp, 2);
    tcp_flush(pcb);
}

// ============================================================
// Dispatch helpers
// ============================================================

static void dispatch_append_complete(net_ctx_t *ctx, struct pbuf *held_pbuf,
                                     const uint8_t *zc_ptr, uint16_t zc_len) {
    int rid = alloc_req_id();
    if (rid < 0) {
        send_error(ctx->pcb, WIRE_ERR_FULL);
        if (held_pbuf) pbuf_free(held_pbuf);
        ctx->wal->slot_free[ctx->cur_slot] = 1;
        return;
    }

    uint16_t total = sizeof(delta_header_t) + ctx->cur_value_len;
    ctx->wal->slot_free[ctx->cur_slot] = 0;

    wal_request_t *req = &ctx->wal->requests[rid];
    req->op       = WAL_OP_APPEND;
    req->slot     = (uint8_t)ctx->cur_slot;
    req->len      = total;
    req->key_hash = ctx->cur_key_hash;
    req->zc_data  = zc_ptr;
    req->zc_len   = zc_len;
    req->zc_pbuf  = held_pbuf;  // Core 0 frees this on DONE
    wal_dmb();
    req->ready    = REQ_PENDING;
    fifo_push_timeout(fifo_signal((uint8_t)rid));
}

static void dispatch_read(net_ctx_t *ctx, uint32_t key_hash) {
    int rid = alloc_req_id();
    if (rid < 0) { send_error(ctx->pcb, WIRE_ERR_FULL); return; }

    wal_request_t *req = &ctx->wal->requests[rid];
    req->op       = WAL_OP_READ;
    req->slot     = 0;
    req->len      = 0;
    req->key_hash = key_hash;
    req->zc_data  = NULL;
    req->zc_pbuf  = NULL;
    wal_dmb();
    req->ready    = REQ_PENDING;
    fifo_push_timeout(fifo_signal((uint8_t)rid));
}

static void dispatch_noop(net_ctx_t *ctx) {
    int rid = alloc_req_id();
    if (rid < 0) { send_error(ctx->pcb, WIRE_ERR_FULL); return; }

    wal_request_t *req = &ctx->wal->requests[rid];
    req->op      = WAL_OP_NOOP;
    req->zc_data = NULL;
    req->zc_pbuf = NULL;
    wal_dmb();
    req->ready   = REQ_PENDING;
    fifo_push_timeout(fifo_signal((uint8_t)rid));
}

// ============================================================
// Handle responses from Core 1 (reverse FIFO)
// ============================================================

static void drain_responses(net_ctx_t *ctx) {
    while (multicore_fifo_rvalid()) {
        uint32_t word = multicore_fifo_pop_blocking();
        uint8_t rid = fifo_req_id(word);
        wal_dmb();  // fence: ensure we see Core 1's writes

        wal_request_t  *req  = &ctx->wal->requests[rid];
        wal_response_t *resp = &ctx->wal->responses[rid];

        switch (req->op) {
        case WAL_OP_APPEND: {
            uint8_t ack[5];
            ack[0] = WIRE_ACK_APPEND;
            memcpy(&ack[1], &resp->seq, 4);
            tcp_send_bytes(ctx->pcb, ack, 5);
            tcp_flush(ctx->pcb);
            // Free the held pbuf now that Core 1 is done reading it
            if (req->zc_pbuf) {
                pbuf_free((struct pbuf *)req->zc_pbuf);
            }
            break;
        }
        case WAL_OP_READ: {
            uint8_t hdr[7];
            hdr[0] = WIRE_ACK_READ;
            memcpy(&hdr[1], &resp->delta_count, 4);
            memcpy(&hdr[5], &resp->result_len, 2);
            tcp_send_bytes(ctx->pcb, hdr, 7);
            if (resp->result_len > 0) {
                tcp_send_bytes(ctx->pcb, ctx->wal->data[resp->result_slot], resp->result_len);
                ctx->wal->slot_free[resp->result_slot] = 1;
            }
            tcp_flush(ctx->pcb);  // single flush for header + data
            break;
        }
        case WAL_OP_NOOP: {
            uint8_t ack = WIRE_ACK_NOOP;
            tcp_send_bytes(ctx->pcb, &ack, 1);
            tcp_flush(ctx->pcb);
            break;
        }
        }

        free_req_id(rid);
        wal_dmb();
        req->ready = REQ_EMPTY;
    }
}

// ============================================================
// Zero-copy TCP receive: phase-based state machine
//
// Phases:
//   RX_OPCODE      → read 1 byte opcode
//   RX_APPEND_HDR  → accumulate 7-byte header into hdr_buf
//   RX_APPEND_DATA → stream value data directly into slot (DMA)
//                     OR pass contiguous pbuf pointer (zero-copy)
//   RX_READ_KEY    → accumulate 4-byte key hash into hdr_buf
// ============================================================

static void process_pbuf_data(net_ctx_t *ctx, struct pbuf *p) {
    // Walk the pbuf chain
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *src = (const uint8_t *)q->payload;
        uint16_t remaining = q->len;
        uint16_t pos = 0;

        while (pos < remaining) {
            switch (ctx->phase) {

            case RX_OPCODE: {
                uint8_t op = src[pos++];
                switch (op) {
                case WIRE_OP_NOOP:
                    dispatch_noop(ctx);
                    break;
                case WIRE_OP_APPEND:
                    ctx->phase = RX_APPEND_HDR;
                    ctx->hdr_pos = 0;
                    break;
                case WIRE_OP_READ:
                    ctx->phase = RX_READ_KEY;
                    ctx->hdr_pos = 0;
                    break;
                default:
                    send_error(ctx->pcb, WIRE_ERR_PROTO);
                    break;
                }
                break;
            }

            case RX_APPEND_HDR: {
                // Accumulate 7 bytes: key_hash(4) + value_len(2) + delta_op(1)
                uint16_t need = 7 - ctx->hdr_pos;
                uint16_t avail = remaining - pos;
                uint16_t copy = (avail < need) ? avail : need;
                memcpy(&ctx->hdr_buf[ctx->hdr_pos], &src[pos], copy);
                ctx->hdr_pos += copy;
                pos += copy;

                if (ctx->hdr_pos == 7) {
                    memcpy(&ctx->cur_key_hash, &ctx->hdr_buf[0], 4);
                    memcpy(&ctx->cur_value_len, &ctx->hdr_buf[4], 2);
                    ctx->cur_delta_op = ctx->hdr_buf[6];

                    if (ctx->cur_value_len > SLOT_SIZE - sizeof(delta_header_t)) {
                        send_error(ctx->pcb, WIRE_ERR_TOOBIG);
                        ctx->phase = RX_OPCODE;
                        break;
                    }

                    // Allocate slot and write delta header
                    ctx->cur_slot = alloc_slot(ctx->wal);
                    if (ctx->cur_slot < 0) {
                        send_error(ctx->pcb, WIRE_ERR_FULL);
                        ctx->phase = RX_OPCODE;
                        break;
                    }

                    delta_header_t dhdr = {
                        .key_hash = ctx->cur_key_hash,
                        .value_len = ctx->cur_value_len,
                        .op = ctx->cur_delta_op,
                        .reserved = 0
                    };
                    memcpy(ctx->wal->data[ctx->cur_slot], &dhdr, sizeof(dhdr));
                    ctx->cur_written = 0;

                    if (ctx->cur_value_len == 0) {
                        // No value data — dispatch immediately
                        dispatch_append_complete(ctx, NULL, NULL, 0);
                        ctx->phase = RX_OPCODE;
                    } else {
                        ctx->phase = RX_APPEND_DATA;

                        // Zero-copy check: if the remaining pbuf data contains
                        // the entire value payload contiguously, pass pointer directly
                        uint16_t pbuf_avail = remaining - pos;
                        if (pbuf_avail >= ctx->cur_value_len) {
                            // Zero-copy! Hold pbuf ref, pass pointer to Core 1
                            pbuf_ref(p);
                            const uint8_t *zc_ptr = &src[pos];
                            dispatch_append_complete(ctx, p, zc_ptr, ctx->cur_value_len);
                            pos += ctx->cur_value_len;
                            ctx->phase = RX_OPCODE;
                        }
                        // else: fall through to RX_APPEND_DATA to copy incrementally
                    }
                }
                break;
            }

            case RX_APPEND_DATA: {
                // Incremental copy: data spans multiple pbufs.
                // DMA for bulk (>64B), memcpy for small fragments.
                // This path is rare: TCP segments are 1460B, values max 508B,
                // so most appends take the zero-copy pbuf path above.
                uint16_t need = ctx->cur_value_len - ctx->cur_written;
                uint16_t avail = remaining - pos;
                uint16_t copy = (avail < need) ? avail : need;

                uint8_t *dst = ctx->wal->data[ctx->cur_slot]
                             + sizeof(delta_header_t)
                             + ctx->cur_written;
                wal_dma_copy(dst, &src[pos], copy);
                ctx->cur_written += copy;
                pos += copy;

                if (ctx->cur_written == ctx->cur_value_len) {
                    // All data in slot — no pbuf hold needed
                    dispatch_append_complete(ctx, NULL, NULL, 0);
                    ctx->phase = RX_OPCODE;
                }
                break;
            }

            case RX_READ_KEY: {
                uint16_t need = 4 - ctx->hdr_pos;
                uint16_t avail = remaining - pos;
                uint16_t copy = (avail < need) ? avail : need;
                memcpy(&ctx->hdr_buf[ctx->hdr_pos], &src[pos], copy);
                ctx->hdr_pos += copy;
                pos += copy;

                if (ctx->hdr_pos == 4) {
                    uint32_t key_hash;
                    memcpy(&key_hash, ctx->hdr_buf, 4);
                    dispatch_read(ctx, key_hash);
                    ctx->phase = RX_OPCODE;
                }
                break;
            }
            } // switch phase
        } // while pos
    } // for pbuf chain
}

// ============================================================
// lwIP callbacks
// ============================================================

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    net_ctx_t *ctx = (net_ctx_t *)arg;
    if (!p || err != ERR_OK) {
        ctx->connected = false;
        if (p) pbuf_free(p);
        if (pcb) tcp_close(pcb);
        ctx->pcb = NULL;
        return ERR_OK;
    }

    process_pbuf_data(ctx, p);

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// ============================================================
// TCP Server: listen, accept, authenticate, then process WAL ops
// ============================================================

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    net_ctx_t *ctx = (net_ctx_t *)arg;

    if (err != ERR_OK || !newpcb) return ERR_VAL;

    // Only one client at a time
    if (ctx->connected && ctx->pcb) {
        printf("[net] Rejecting connection — already have a client\n");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    printf("[net] Client connected from %s\n",
           ip4addr_ntoa(&newpcb->remote_ip));

    ctx->pcb = newpcb;
    ctx->connected = true;
    ctx->conn_state = CONN_READY;
    ctx->phase = RX_OPCODE;
    ctx->hdr_pos = 0;

    tcp_arg(newpcb, ctx);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);

    printf("[net] TCP WAL client connected\n");
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    net_ctx_t *ctx = (net_ctx_t *)arg;
    printf("[net] TCP error: %d\n", err);
    ctx->connected = false;
    ctx->conn_state = CONN_READY;
    ctx->pcb = NULL;
}

static bool tcp_start_listen(net_ctx_t *ctx) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return false;

    err_t e = tcp_bind(pcb, IP_ADDR_ANY, WAL_LISTEN_PORT);
    if (e != ERR_OK) {
        printf("[net] Bind failed: %d\n", e);
        tcp_close(pcb);
        return false;
    }

    ctx->listen_pcb = tcp_listen(pcb);
    if (!ctx->listen_pcb) {
        printf("[net] Listen failed\n");
        tcp_close(pcb);
        return false;
    }

    tcp_arg(ctx->listen_pcb, ctx);
    tcp_accept(ctx->listen_pcb, on_accept);

    printf("[net] Listening on port %d\n", WAL_LISTEN_PORT);
    return true;
}

// ============================================================
// LCD Dashboard — refreshed every 30s from Core 0 poll loop
// (LCD SPI must only be driven from Core 0)
// ============================================================

#define LCD_REFRESH_MS 10000u
#define HTTP_UI_QUIET_MS 150u
#define DASH_LINES 4
#define DASH_LINE_MAX 48

// Core 0 LCD state — Scratch X for contention-free dashboard updates
static uint32_t __scratch_x("lcd") g_lcd_last_ms = 0;
static char __scratch_x("lcd") g_dash_prev[DASH_LINES][DASH_LINE_MAX];
static bool __scratch_x("lcd") g_dash_first = true;

// Draw a dashboard line only if the text changed since last refresh.
static void dash_line(int idx, uint16_t x, uint16_t y, const char *text,
                      uint16_t fg, uint16_t bg, uint8_t size) {
    if (!g_dash_first && strcmp(text, g_dash_prev[idx]) == 0) return;
    char padded[DASH_LINE_MAX];
    int len = 0;
    while (text[len] && len < DASH_LINE_MAX - 1) { padded[len] = text[len]; len++; }
    int prev_len = (int)strlen(g_dash_prev[idx]);
    while (len < prev_len && len < DASH_LINE_MAX - 1) { padded[len++] = ' '; }
    padded[len] = '\0';
    lcd_draw_string(x, y, padded, fg, bg, size);
    memcpy(g_dash_prev[idx], text, strlen(text) + 1);
}

static void lcd_refresh_dashboard(wal_state_t *wal) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - g_lcd_last_ms) < LCD_REFRESH_MS && g_lcd_last_ms != 0) return;
    g_lcd_last_ms = now;

    kv_stats_t st = kv_stats();
    const char *ip_text = ip4addr_ntoa(netif_ip4_addr(netif_list));
    char line[64];

    if (g_dash_first) {
        lcd_clear(COLOR_BLACK);
    }

    snprintf(line, sizeof(line), "WIFI: %s", WIFI_SSID);
    dash_line(0, 20, 10, line, COLOR_CYAN, COLOR_BLACK, 2);

    snprintf(line, sizeof(line), "HTTP: %s:80", ip_text);
    dash_line(1, 20, 40, line, COLOR_WHITE, COLOR_BLACK, 2);

    snprintf(line, sizeof(line), "FLASH: %lu PG FREE", (unsigned long)st.free);
    dash_line(2, 20, 70, line, st.free == 0 ? COLOR_RED : COLOR_GREEN, COLOR_BLACK, 2);

    if (kvsd_ready()) {
        kvsd_stats_t sds = kvsd_stats();
        uint32_t used_pct = sds.max_cards ? (sds.active * 100 / sds.max_cards) : 0;
        snprintf(line, sizeof(line), "SD: %lu/%lu (%lu%%)",
                 (unsigned long)sds.active, (unsigned long)sds.max_cards,
                 (unsigned long)used_pct);
        dash_line(3, 20, 100, line, used_pct > 90 ? COLOR_RED : COLOR_GREEN, COLOR_BLACK, 2);
    } else {
        dash_line(3, 20, 100, "SD: NOT READY", COLOR_RED, COLOR_BLACK, 2);
    }

    g_dash_first = false;
}

// ============================================================
// Core 0 Main Loop
// ============================================================

void net_core_run(wal_state_t *wal) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    memset(req_id_bitmap, 0, sizeof(req_id_bitmap));
    g_ctx.wal = wal;

    if (cyw43_arch_init()) {
        printf("[net] CYW43 init failed\n");
        while (1) tight_loop_contents();
    }
    cyw43_arch_enable_sta_mode();

    printf("[net] Connecting to WiFi '%s'...\n", WIFI_SSID);
    int werr;
    for (int attempt = 0; attempt < 5; attempt++) {
        werr = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, WIFI_TIMEOUT_MS);
        if (!werr) break;
        printf("[net] WiFi attempt %d failed (err %d), retrying...\n", attempt + 1, werr);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(500);
    }
    if (werr) {
        printf("[net] WiFi failed after 5 attempts\n");
        while (1) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(200);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(200);
        }
    }

    if (!netif_list) {
        printf("[net] FATAL: no network interface\n");
        while (1) tight_loop_contents();
    }

    printf("[net] WiFi OK, waiting for DHCP...\n");
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    for (uint32_t w = 0; w < 200; w++) {   // 20s timeout
        if (dhcp_supplied_address(netif_list) &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif_list))) break;
        cyw43_arch_poll();
        sleep_ms(100);
    }
    if (!dhcp_supplied_address(netif_list) ||
        ip4_addr_isany_val(*netif_ip4_addr(netif_list))) {
        printf("[net] DHCP timeout\n");
        while (1) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(200);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(200);
        }
    }
    printf("[net] DHCP IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));

    // Start HTTP server on port 80
    web_server_init(g_ctx.wal);

    // Start UDP WAL listener on port 8002
    udp_wal_init(g_ctx.wal);

    // Enable hardware watchdog — 8s timeout, reboot if poll loop stalls
    watchdog_enable(8000, true);

    // Main poll loop
    uint32_t hb_last = 0;
    bool hb_on = false;
    bool wal_tcp_started = false;
    uint32_t poll_count = 0;
    uint32_t core1_hb_prev = 0;
    uint32_t core1_stall_count = 0;
    while (true) {
        wal->core0_heartbeat++;
        poll_count++;
        watchdog_update();   // feed hardware watchdog every poll cycle
        cyw43_arch_poll();

        // Lazy-start WAL TCP listener after WiFi is stable (~100 poll cycles)
        if (!wal_tcp_started && poll_count > 100) {
            if (tcp_start_listen(&g_ctx)) {
                printf("[net] WAL TCP on port %d\n", WAL_LISTEN_PORT);
            }
            wal_tcp_started = true;
        }

        if (g_ctx.connected) {
            drain_responses(&g_ctx);
        }
        udp_wal_poll();
        // Heavy I/O (LCD redraw, SD flush) only when HTTP is quiet
        // AND at most once per second to avoid starving cyw43_arch_poll
        if (!web_server_recent_activity(HTTP_UI_QUIET_MS)) {
            lcd_refresh_dashboard(wal);  // already internally throttled to 10s
            flush_cardinality_one();
        }
        // Heartbeat indicator — top-left dot toggles every 1s
        {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - hb_last >= 1000) {
                hb_last = now;
                hb_on = !hb_on;
                lcd_draw_string(0, 0, hb_on ? "*" : " ", COLOR_MAGENTA, COLOR_BLACK, 2);
                // Flush SD index at most once per second (was every poll cycle)
                if (kvsd_dirty()) kvsd_flush();
                // Core 1 stall detection (skip during OTA halt)
                if (!wal->ota_halt_core1) {
                    if (wal->core1_heartbeat == core1_hb_prev) {
                        core1_stall_count++;
                        if (core1_stall_count >= 5)
                            printf("[net] WARN: Core 1 stalled for %us\n", core1_stall_count);
                    } else {
                        core1_stall_count = 0;
                    }
                    core1_hb_prev = wal->core1_heartbeat;
                }
            }
        }
        // Tight spin — no sleep. SD writes are the natural throttle.
        // cyw43_arch_poll() yields to WiFi driver as needed.
    }
}
