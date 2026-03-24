#include "net_core.h"
#include "wal_defs.h"
#include "wal_fence.h"
#include "wal_dma.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "lwip/tcp.h"

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

typedef enum {
    RX_OPCODE,       // waiting for 1-byte opcode
    RX_APPEND_HDR,   // accumulating 7-byte append header
    RX_APPEND_DATA,  // streaming value data (zero-copy or to slot)
    RX_READ_KEY,     // accumulating 4-byte key hash
} rx_phase_t;

typedef struct {
    wal_state_t    *wal;
    struct tcp_pcb *pcb;
    bool            connected;

    // Frame parser state
    rx_phase_t phase;
    uint8_t    hdr_buf[8];   // small header accumulator (max 7 bytes)
    uint8_t    hdr_pos;

    // Current APPEND in progress
    uint32_t   cur_key_hash;
    uint16_t   cur_value_len;
    uint8_t    cur_delta_op;
    int        cur_slot;     // allocated slot for this append
    uint16_t   cur_written;  // bytes written to slot so far
} net_ctx_t;

static net_ctx_t g_ctx;

// ============================================================
// Find a free buffer slot
// ============================================================

static int alloc_slot(wal_state_t *wal) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (wal->slot_free[i]) return i;
    }
    return -1;
}

// ============================================================
// TCP send helpers
// ============================================================

static void tcp_send_bytes(struct tcp_pcb *pcb, const uint8_t *data, uint16_t len) {
    if (!pcb) return;
    tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void send_error(struct tcp_pcb *pcb, uint8_t code) {
    uint8_t resp[2] = {WIRE_ERROR, code};
    tcp_send_bytes(pcb, resp, 2);
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
    multicore_fifo_push_blocking(fifo_signal((uint8_t)rid));
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
    multicore_fifo_push_blocking(fifo_signal((uint8_t)rid));
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
    multicore_fifo_push_blocking(fifo_signal((uint8_t)rid));
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
            break;
        }
        case WAL_OP_NOOP: {
            uint8_t ack = WIRE_ACK_NOOP;
            tcp_send_bytes(ctx->pcb, &ack, 1);
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
                // Incremental copy: data spans multiple pbufs, DMA to slot
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

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    net_ctx_t *ctx = (net_ctx_t *)arg;
    if (err != ERR_OK) { ctx->connected = false; return err; }
    printf("[net] Connected to %s:%d\n", WAL_HOST, WAL_PORT);
    ctx->connected = true;
    ctx->phase = RX_OPCODE;
    ctx->hdr_pos = 0;
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    net_ctx_t *ctx = (net_ctx_t *)arg;
    printf("[net] TCP error: %d\n", err);
    ctx->connected = false;
    ctx->pcb = NULL;
}

static bool tcp_connect_to_host(net_ctx_t *ctx) {
    ctx->pcb = tcp_new();
    if (!ctx->pcb) return false;
    tcp_arg(ctx->pcb, ctx);
    tcp_recv(ctx->pcb, on_recv);
    tcp_err(ctx->pcb, on_err);

    ip_addr_t addr;
    if (!ip4addr_aton(WAL_HOST, &addr)) { tcp_abort(ctx->pcb); ctx->pcb = NULL; return false; }

    err_t e = tcp_connect(ctx->pcb, &addr, WAL_PORT, on_connected);
    if (e != ERR_OK) { tcp_abort(ctx->pcb); ctx->pcb = NULL; return false; }

    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!ctx->connected && ctx->pcb) {
        cyw43_arch_poll();
        sleep_ms(1);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            if (ctx->pcb) { tcp_abort(ctx->pcb); ctx->pcb = NULL; }
            return false;
        }
    }
    return ctx->connected;
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
    int werr = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, WIFI_TIMEOUT_MS);
    if (werr) {
        printf("[net] WiFi failed (err %d)\n", werr);
        while (1) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(200);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(200);
        }
    }
    printf("[net] WiFi OK, IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));

    while (true) {
        if (!g_ctx.connected) {
            printf("[net] Connecting to %s:%d...\n", WAL_HOST, WAL_PORT);
            if (!tcp_connect_to_host(&g_ctx)) {
                printf("[net] Retry in 2s...\n");
                sleep_ms(2000);
                continue;
            }
        }

        // Poll network AND drain Core 1 responses
        cyw43_arch_poll();
        drain_responses(&g_ctx);
        sleep_ms(1);
    }
}
