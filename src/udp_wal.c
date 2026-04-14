#include "udp_wal.h"
#include "wal_defs.h"
#include "kv_sd.h"
#include "sd_card.h"
#include "crypto.h"
#include "net_core.h"

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include <string.h>
#include <stdio.h>

// (SD ring buffer removed — SRAM-only buffering is more efficient
//  since the bottleneck is SD write speed, not buffer space)

// ============================================================
// Session table
// ============================================================

typedef struct {
    uint64_t session_id;
    ip_addr_t addr;
    uint16_t  port;
    uint32_t  last_seq;
    uint32_t  last_seen_ms;
    bool      active;
    bool      encrypted;     // true after key exchange
    uint8_t   key[32];       // derived session key (ChaCha20-Poly1305)
    uint8_t   client_random[16];
    uint8_t   server_random[16];
} udp_session_t;

static udp_session_t g_sessions[UDP_WAL_MAX_SESSIONS];
static struct udp_pcb *g_pcb;
static wal_state_t *g_wal;
static uint32_t g_epoch;

// ============================================================
// Helpers
// ============================================================

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint64_t gen_session_id(void) {
    return ((uint64_t)get_rand_32() << 32) | get_rand_32();
}

static udp_session_t *find_session(uint64_t sid) {
    for (int i = 0; i < UDP_WAL_MAX_SESSIONS; i++) {
        if (g_sessions[i].active && g_sessions[i].session_id == sid)
            return &g_sessions[i];
    }
    return NULL;
}

static udp_session_t *alloc_session(void) {
    // Find free slot
    for (int i = 0; i < UDP_WAL_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) return &g_sessions[i];
    }
    // Evict oldest
    uint32_t oldest = UINT32_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < UDP_WAL_MAX_SESSIONS; i++) {
        if (g_sessions[i].last_seen_ms < oldest) {
            oldest = g_sessions[i].last_seen_ms;
            oldest_idx = i;
        }
    }
    g_sessions[oldest_idx].active = false;
    return &g_sessions[oldest_idx];
}

// Read/write little-endian helpers
static inline uint16_t rd16(const uint8_t *p) { return p[0] | ((uint16_t)p[1]<<8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4)<<32); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p,(uint32_t)v); wr32(p+4,(uint32_t)(v>>32)); }

// ============================================================
// Send response datagram (plaintext — for HELLO/RESUME)
// ============================================================

static void udp_send_to(const ip_addr_t *addr, uint16_t port,
                        uint64_t session_id, uint8_t msg_type,
                        const uint8_t *payload, uint16_t payload_len) {
    uint16_t total = UDP_WAL_HDR_SIZE + payload_len;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, total, PBUF_RAM);
    if (!p) return;
    uint8_t *buf = (uint8_t *)p->payload;

    wr64(buf, session_id);
    wr16(buf + 8, (uint16_t)g_epoch);
    wr32(buf + 10, 0);
    buf[14] = msg_type;
    if (payload_len > 0) memcpy(buf + 15, payload, payload_len);

    udp_sendto(g_pcb, p, addr, port);
    pbuf_free(p);
}

// Encrypted send — header is AAD, payload is encrypted + 16-byte tag appended
static void udp_send_encrypted(udp_session_t *s, uint8_t msg_type,
                                const uint8_t *payload, uint16_t payload_len) {
    if (!s->encrypted) {
        udp_send_to(&s->addr, s->port, s->session_id, msg_type, payload, payload_len);
        return;
    }

    // Build nonce from session_id(8) + seq(4)
    static uint32_t s_send_seq = 0;
    s_send_seq++;
    uint8_t nonce[12];
    wr64(nonce, s->session_id);
    wr32(nonce + 8, s_send_seq);

    // Header = AAD (not encrypted)
    uint8_t hdr[UDP_WAL_HDR_SIZE];
    wr64(hdr, s->session_id);
    wr16(hdr + 8, (uint16_t)g_epoch);
    wr32(hdr + 10, s_send_seq);
    hdr[14] = msg_type;

    // Encrypt payload
    uint8_t ct[UDP_WAL_MAX_PAYLOAD + 16];
    uint32_t ct_len = aead_encrypt(ct, payload, payload_len,
                                    hdr, UDP_WAL_HDR_SIZE,
                                    s->key, nonce);

    uint16_t total = UDP_WAL_HDR_SIZE + ct_len;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, total, PBUF_RAM);
    if (!p) return;
    uint8_t *buf = (uint8_t *)p->payload;
    memcpy(buf, hdr, UDP_WAL_HDR_SIZE);
    memcpy(buf + UDP_WAL_HDR_SIZE, ct, ct_len);

    udp_sendto(g_pcb, p, &s->addr, s->port);
    pbuf_free(p);
}

// ============================================================
// WAL FIFO helpers (same pattern as web_server.c)
// ============================================================

static int wal_alloc_req_id(void) {
    for (int i = 0; i < REQ_RING_SIZE; i++) {
        if (g_wal->requests[i].ready == REQ_EMPTY) return i;
    }
    return -1;
}

static int wal_alloc_slot(void) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (g_wal->slot_free[i]) {
            g_wal->slot_free[i] = 0;
            return i;
        }
    }
    return -1;
}

// Direct KV write — called from UDP callback, runs on Core 0.
// Uses kv_store unified path (flash for packs 0-1, SD for packs 2+).
// No WAL FIFO needed — cooperative single-threaded on Core 0.
#define KV_STORE_REDIRECT
#include "kv_store.h"

static bool direct_put(uint32_t key, const uint8_t *data, uint16_t len) {
    return kv_store_put(key, data, len);
}

static bool direct_get(uint32_t key, uint8_t *out, uint16_t *len) {
    return kv_store_get_copy(key, out, len, NULL);
}

// ============================================================
// Handle HELLO — create new session
// ============================================================

static void handle_hello(const ip_addr_t *addr, uint16_t port,
                         const uint8_t *payload, uint16_t len) {
    if (len < 16) return;

    udp_session_t *s = alloc_session();
    s->session_id = gen_session_id();
    s->addr = *addr;
    s->port = port;
    s->last_seq = 0;
    s->last_seen_ms = now_ms();
    s->active = true;

    // Store client random
    memcpy(s->client_random, payload, 16);

    // Generate server random
    for (int i = 0; i < 16; i++)
        s->server_random[i] = (uint8_t)(get_rand_32() >> ((i%4)*8));

    // Derive session key: HKDF(PSK, client_random || server_random || epoch)
    {
        static const uint8_t psk[] = AUTH_PSK;
        uint8_t ikm[36]; // client_random(16) + server_random(16) + epoch(4)
        memcpy(ikm, s->client_random, 16);
        memcpy(ikm + 16, s->server_random, 16);
        wr32(ikm + 32, g_epoch);
        hkdf_sha256(s->key, 32, ikm, 36, psk, sizeof(psk),
                     (const uint8_t *)"picowal-udp", 11);
        s->encrypted = true;
    }

    // Response: [session_id:8][server_random:16][epoch:4]
    uint8_t resp[28];
    wr64(resp, s->session_id);
    memcpy(resp + 8, s->server_random, 16);
    wr32(resp + 24, g_epoch);

    udp_send_to(addr, port, s->session_id, UMSG_HELLO_OK, resp, 28);
}

// ============================================================
// Handle RESUME — reconnect existing session
// ============================================================

static void handle_resume(const ip_addr_t *addr, uint16_t port,
                          uint64_t session_id) {
    udp_session_t *s = find_session(session_id);
    if (!s) {
        udp_send_to(addr, port, session_id, UMSG_RESUME_FAIL, NULL, 0);
        return;
    }
    s->addr = *addr;
    s->port = port;
    s->last_seen_ms = now_ms();

    uint8_t resp[4];
    wr32(resp, g_epoch);
    udp_send_to(addr, port, session_id, UMSG_RESUME_OK, resp, 4);
}

// ============================================================
// Deferred write queue — callback stashes, poll loop executes
// ============================================================

typedef struct {
    uint32_t key;
    uint16_t len;
    uint8_t  data[128];     // cards are typically <50 bytes
} pending_card_t;

typedef struct {
    bool       active;
    uint64_t   session_id;
    ip_addr_t  addr;
    uint16_t   port;
    uint16_t   batch_seq;
    uint8_t    count;
    uint8_t    durability;
    uint8_t    next;          // next card to write
    uint32_t   bitmap;        // result bitmap
    pending_card_t cards[UDP_WAL_MAX_BATCH];
} deferred_batch_t;

#define DEFERRED_QUEUE_SIZE 16
static deferred_batch_t g_deferred[DEFERRED_QUEUE_SIZE];

// ============================================================
// Raw datagram ring — fast SRAM buffer for callback overflow
// Just memcpy, no parsing or SD I/O in callback context
// 64 slots × 512 bytes = 32KB (max available SRAM)
// ============================================================

#define RAW_RING_SIZE  48
#define RAW_RING_MTU   512
static uint8_t  g_raw_ring[RAW_RING_SIZE][RAW_RING_MTU];
static uint16_t g_raw_ring_len[RAW_RING_SIZE];
static volatile uint32_t g_raw_write = 0;
static volatile uint32_t g_raw_read = 0;

static bool raw_ring_push(const uint8_t *data, uint16_t len) {
    uint32_t next = (g_raw_write + 1) % RAW_RING_SIZE;
    if (next == g_raw_read) return false;
    uint16_t clen = len > RAW_RING_MTU ? RAW_RING_MTU : len;
    memcpy(g_raw_ring[g_raw_write], data, clen);
    g_raw_ring_len[g_raw_write] = clen;
    g_raw_write = next;
    return true;
}

// ============================================================
// Handle BATCH_WRITE — parse and queue, don't do SD I/O
// ============================================================

static void handle_batch_write(udp_session_t *s,
                               const uint8_t *payload, uint16_t len) {
    if (len < 4) return;
    uint16_t batch_seq = rd16(payload);
    uint8_t count = payload[2];
    uint8_t durability = payload[3];
    if (count == 0 || count > UDP_WAL_MAX_BATCH) return;

    // Find free deferred slot
    deferred_batch_t *db = NULL;
    for (int i = 0; i < DEFERRED_QUEUE_SIZE; i++) {
        if (!g_deferred[i].active) { db = &g_deferred[i]; break; }
    }
    if (!db) {
        // Backpressure — all slots full
        uint8_t bp[1] = { DEFERRED_QUEUE_SIZE };
        udp_send_encrypted(s, UMSG_BACKPRESSURE, bp, 1);
        return;
    }

    db->active = true;
    db->session_id = s->session_id;
    db->addr = s->addr;
    db->port = s->port;
    db->batch_seq = batch_seq;
    db->durability = durability;
    db->next = 0;
    db->bitmap = 0;
    db->count = 0;

    // Parse cards into the deferred batch
    uint16_t off = 4;
    for (uint8_t i = 0; i < count && off + 8 <= len; i++) {
        uint16_t pack = rd16(payload + off);
        uint32_t card = rd32(payload + off + 2);
        uint16_t clen = rd16(payload + off + 6);
        off += 8;
        if (off + clen > len || clen > 128) break;

        db->cards[i].key = ((uint32_t)(pack & 0x3FF) << 22) | (card & 0x3FFFFF);
        db->cards[i].len = clen;
        memcpy(db->cards[i].data, payload + off, clen);
        db->count++;
        off += clen;
    }
}

// ============================================================
// Handle READ
// ============================================================

static void handle_read(udp_session_t *s,
                        const uint8_t *payload, uint16_t len) {
    if (len < 6) return;
    uint16_t pack = rd16(payload);
    uint32_t card = rd32(payload + 2);
    uint32_t key = ((uint32_t)(pack & 0x3FF) << 22) | (card & 0x3FFFFF);

    uint8_t buf[512];
    uint16_t blen = sizeof(buf);
    bool ok = direct_get(key, buf, &blen);

    if (ok) {
        uint8_t resp[8 + 512];
        wr16(resp, pack);
        wr32(resp + 2, card);
        wr16(resp + 6, blen);
        memcpy(resp + 8, buf, blen);
        udp_send_encrypted(s, UMSG_DATA, resp, 8 + blen);
    } else {
        uint8_t resp[6];
        wr16(resp, pack);
        wr32(resp + 2, card);
        udp_send_encrypted(s, UMSG_NOT_FOUND, resp, 6);
    }
}

// ============================================================
// Main recv callback
// ============================================================

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    if (!p || p->tot_len < UDP_WAL_HDR_SIZE) { if (p) pbuf_free(p); return; }

    // Overflow: if SRAM deferred queue full and this is a BATCH_WRITE,
    // parse cards directly into flash ring buffer instead of dropping
    {
        bool any_free = false;
        for (int i = 0; i < DEFERRED_QUEUE_SIZE; i++) {
            if (!g_deferred[i].active) { any_free = true; break; }
        }
        uint8_t peek_type = 0;
        pbuf_copy_partial(p, &peek_type, 1, 14);
        if (!any_free && peek_type == UMSG_BATCH_WRITE) {
            // SRAM deferred queue full — push raw datagram to SRAM ring
            // (fast memcpy, parsed + written to SD in poll loop)
            uint16_t total = p->tot_len;
            uint8_t raw[RAW_RING_MTU];
            uint16_t clen = total > RAW_RING_MTU ? RAW_RING_MTU : total;
            pbuf_copy_partial(p, raw, clen, 0);
            pbuf_free(p);
            raw_ring_push(raw, clen);
            return;
        }
    }

    uint8_t hdr[UDP_WAL_HDR_SIZE];
    pbuf_copy_partial(p, hdr, UDP_WAL_HDR_SIZE, 0);

    uint64_t session_id = rd64(hdr);
    uint16_t epoch = rd16(hdr + 8);
    uint32_t seq = rd32(hdr + 10);
    uint8_t msg_type = hdr[14];

    // Copy payload after header
    uint16_t payload_len = p->tot_len - UDP_WAL_HDR_SIZE;
    uint8_t payload[UDP_WAL_MAX_PAYLOAD];
    if (payload_len > sizeof(payload)) payload_len = sizeof(payload);
    pbuf_copy_partial(p, payload, payload_len, UDP_WAL_HDR_SIZE);
    pbuf_free(p);

    // HELLO is special — no session yet
    if (msg_type == UMSG_HELLO) {
        handle_hello(addr, port, payload, payload_len);
        return;
    }

    // All other messages need a valid session
    udp_session_t *s = find_session(session_id);

    if (msg_type == UMSG_RESUME) {
        handle_resume(addr, port, session_id);
        return;
    }

    if (!s) {
        udp_send_to(addr, port, session_id, UMSG_RESUME_FAIL, NULL, 0);
        return;
    }

    // Decrypt if session is encrypted — ALL packets must be authenticated
    if (s->encrypted) {
        if (payload_len < 16) return;  // too short for auth tag, drop
        uint8_t nonce[12];
        wr64(nonce, session_id);
        wr32(nonce + 8, seq);
        uint8_t decrypted[UDP_WAL_MAX_PAYLOAD];
        uint32_t pt_len = aead_decrypt(decrypted, payload, payload_len,
                                        hdr, UDP_WAL_HDR_SIZE,
                                        s->key, nonce);
        if (pt_len == 0) return;  // auth failed, drop silently
        memcpy(payload, decrypted, pt_len);
        payload_len = (uint16_t)pt_len;
    }

    // Replay check
    if (seq <= s->last_seq && s->last_seq > 0) {
        return;  // drop silently
    }
    s->last_seq = seq;
    s->last_seen_ms = now_ms();

    switch (msg_type) {
    case UMSG_BATCH_WRITE:
        handle_batch_write(s, payload, payload_len);
        break;
    case UMSG_READ:
        handle_read(s, payload, payload_len);
        break;
    default:
        break;
    }
}

// ============================================================
// Poll — check pending batches for committed responses
// ============================================================

void udp_wal_poll(void) {
    // Process ONE card from the first active deferred batch.
    // One SD write per poll keeps cyw43_arch_poll() responsive.
    for (int i = 0; i < DEFERRED_QUEUE_SIZE; i++) {
        deferred_batch_t *db = &g_deferred[i];
        if (!db->active) continue;

        if (db->next < db->count) {
            pending_card_t *c = &db->cards[db->next];
            if (direct_put(c->key, c->data, c->len)) {
                db->bitmap |= (1u << db->next);
            }
            db->next++;
        }

        if (db->next >= db->count) {
            if (db->durability >= UDUR_ACK_QUEUED) {
                uint8_t ack[9];
                wr16(ack, db->batch_seq);
                ack[2] = db->count;
                wr32(ack + 3, db->bitmap);
                wr16(ack + 7, 0);
                uint8_t msg_type = (db->durability >= UDUR_ACK_DURABLE) ?
                    UMSG_BATCH_COMMITTED : UMSG_BATCH_QUEUED;
                udp_session_t *ack_s = find_session(db->session_id);
                if (ack_s) {
                    udp_send_encrypted(ack_s, msg_type, ack, 9);
                } else {
                    udp_send_to(&db->addr, db->port, db->session_id, msg_type, ack, 9);
                }
            }
            db->active = false;
        }
        return;  // one card per poll
    }

    // No deferred work — drain one raw ring entry
    if (g_raw_read != g_raw_write) {
        uint8_t *raw = g_raw_ring[g_raw_read];
        uint16_t rlen = g_raw_ring_len[g_raw_read];
        g_raw_read = (g_raw_read + 1) % RAW_RING_SIZE;

        if (rlen >= UDP_WAL_HDR_SIZE + 8) {
            uint8_t *payload = raw + UDP_WAL_HDR_SIZE;
            uint16_t plen = rlen - UDP_WAL_HDR_SIZE;
            uint8_t count = payload[2];
            uint16_t off = 4;
            for (uint8_t ci = 0; ci < count && off + 8 <= plen; ci++) {
                uint16_t pack = rd16(payload + off);
                uint32_t card = rd32(payload + off + 2);
                uint16_t clen = rd16(payload + off + 6);
                off += 8;
                if (off + clen > plen || clen > 128) break;
                uint32_t key = ((uint32_t)(pack & 0x3FF) << 22) | (card & 0x3FFFFF);
                direct_put(key, payload + off, clen);
                off += clen;
            }
        }
        return;
    }
}

// ============================================================
// Init
// ============================================================

void udp_wal_init(wal_state_t *wal) {
    g_wal = wal;
    g_epoch = now_ms();
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(g_deferred, 0, sizeof(g_deferred));

    g_pcb = udp_new();
    if (!g_pcb) {
        printf("[udp] Failed to create PCB\n");
        return;
    }

    err_t err = udp_bind(g_pcb, IP_ADDR_ANY, UDP_WAL_PORT);
    if (err != ERR_OK) {
        printf("[udp] Bind failed: %d\n", err);
        udp_remove(g_pcb);
        g_pcb = NULL;
        return;
    }

    udp_recv(g_pcb, udp_recv_cb, NULL);
    printf("[udp] WAL listener on port %d (epoch %lu)\n",
           UDP_WAL_PORT, (unsigned long)g_epoch);
}
