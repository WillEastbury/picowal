#include "wal_engine.h"
#include "wal_defs.h"
#include "wal_fence.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include <string.h>
#include <stdio.h>

static wal_state_t *g_wal;

// ============================================================
// WAL Index helpers
// ============================================================

static int wal_alloc_entry(void) {
    for (uint32_t i = 0; i < WAL_MAX_ENTRIES; i++) {
        if (g_wal->entries[i].seq == 0) return (int)i;
    }
    return -1;
}

// Find a free buffer slot
static int wal_alloc_slot(void) {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (g_wal->slot_free[i]) return i;
    }
    return -1;
}

// ============================================================
// APPEND: register delta in WAL index, respond with seq
// ============================================================

static void wal_do_append(uint8_t req_id) {
    wal_request_t  *req  = &g_wal->requests[req_id];
    wal_response_t *resp = &g_wal->responses[req_id];

    memset(resp, 0, sizeof(*resp));

    if (req->len < sizeof(delta_header_t)) {
        resp->status = WAL_RESP_ERR;
        wal_dmb();
        req->ready = REQ_DONE;
        multicore_fifo_push_blocking(fifo_signal(req_id));
        return;
    }

    // If zero-copy pointer is set, copy value data into slot now
    // (before we signal DONE, which frees the pbuf on Core 0)
    if (req->zc_data && req->zc_len > 0) {
        memcpy(g_wal->data[req->slot] + sizeof(delta_header_t),
               req->zc_data, req->zc_len);
    }

    delta_header_t hdr;
    memcpy(&hdr, g_wal->data[req->slot], sizeof(hdr));

    int idx = wal_alloc_entry();
    if (idx < 0) {
        resp->status = WAL_RESP_ERR;
        g_wal->slot_free[req->slot] = 1;
        wal_dmb();
        req->ready = REQ_DONE;
        multicore_fifo_push_blocking(fifo_signal(req_id));
        return;
    }

    wal_entry_t *e = &g_wal->entries[idx];
    e->seq      = g_wal->next_seq++;
    e->key_hash = hdr.key_hash;
    e->slot     = req->slot;
    e->len      = req->len;
    e->flags    = WAL_ENTRY_ACTIVE;
    if (hdr.op == DELTA_OP_DELETE) e->flags |= WAL_ENTRY_TOMBSTONE;

    g_wal->entry_count++;

    resp->status = WAL_RESP_OK;
    resp->seq    = e->seq;
    wal_dmb();
    req->ready = REQ_DONE;
    multicore_fifo_push_blocking(fifo_signal(req_id));
}

// ============================================================
// READ: collect all deltas for key, compact into one response
// slot, send back via reverse FIFO
// ============================================================

static void wal_do_read(uint8_t req_id) {
    wal_request_t  *req  = &g_wal->requests[req_id];
    wal_response_t *resp = &g_wal->responses[req_id];

    memset(resp, 0, sizeof(*resp));

    uint32_t key_hash = req->key_hash;

    // Collect matching entries sorted by seq (simple selection)
    // Temp arrays on stack — 192 entries max, ~1KB
    uint32_t match_seqs[WAL_MAX_ENTRIES];
    uint8_t  match_idxs[WAL_MAX_ENTRIES];
    uint32_t match_count = 0;

    for (uint32_t i = 0; i < WAL_MAX_ENTRIES; i++) {
        wal_entry_t *e = &g_wal->entries[i];
        if (e->seq != 0 && e->key_hash == key_hash &&
            (e->flags & WAL_ENTRY_ACTIVE) && !(e->flags & WAL_ENTRY_COMPACTED)) {
            match_seqs[match_count] = e->seq;
            match_idxs[match_count] = (uint8_t)i;
            match_count++;
        }
    }

    if (match_count == 0) {
        resp->status      = WAL_RESP_OK;
        resp->result_slot = 0;
        resp->result_len  = 0;
        resp->delta_count = 0;
        wal_dmb();  // fence: commit before ownership transfer
    req->ready = REQ_DONE;
        multicore_fifo_push_blocking(fifo_signal(req_id));
        return;
    }

    // Sort by seq ascending (simple insertion sort, N ≤ 192)
    for (uint32_t i = 1; i < match_count; i++) {
        uint32_t s = match_seqs[i];
        uint8_t  x = match_idxs[i];
        int j = (int)i - 1;
        while (j >= 0 && match_seqs[j] > s) {
            match_seqs[j + 1] = match_seqs[j];
            match_idxs[j + 1] = match_idxs[j];
            j--;
        }
        match_seqs[j + 1] = s;
        match_idxs[j + 1] = x;
    }

    // Allocate a response slot and pack compacted deltas into it
    // Format: [delta_header_t + value] for each delta, concatenated
    int rslot = wal_alloc_slot();
    if (rslot < 0) {
        resp->status = WAL_RESP_ERR;
        wal_dmb();  // fence: commit before ownership transfer
    req->ready = REQ_DONE;
        multicore_fifo_push_blocking(fifo_signal(req_id));
        return;
    }
    g_wal->slot_free[rslot] = 0;

    uint8_t *out = g_wal->data[rslot];
    uint16_t out_pos = 0;

    for (uint32_t i = 0; i < match_count; i++) {
        wal_entry_t *e = &g_wal->entries[match_idxs[i]];
        uint16_t elen = e->len;

        if (out_pos + elen > SLOT_SIZE) break;  // truncate if won't fit

        memcpy(&out[out_pos], g_wal->data[e->slot], elen);
        out_pos += elen;
    }

    resp->status      = WAL_RESP_OK;
    resp->result_slot = (uint8_t)rslot;
    resp->result_len  = out_pos;
    resp->delta_count = match_count;
    resp->seq         = match_seqs[match_count - 1];

    wal_dmb();  // fence: commit before ownership transfer
    req->ready = REQ_DONE;
    multicore_fifo_push_blocking(fifo_signal(req_id));
}

// ============================================================
// Background Compaction (runs when FIFO is empty)
// ============================================================

static uint32_t compact_cursor = 0;

static void wal_compact_step(void) {
    wal_entry_t *anchor = NULL;
    uint32_t anchor_idx = WAL_MAX_ENTRIES;

    for (uint32_t i = 0; i < WAL_MAX_ENTRIES; i++) {
        uint32_t idx = (compact_cursor + i) % WAL_MAX_ENTRIES;
        wal_entry_t *e = &g_wal->entries[idx];
        if (e->seq != 0 && (e->flags & WAL_ENTRY_ACTIVE) &&
            !(e->flags & WAL_ENTRY_COMPACTED)) {
            anchor = e;
            anchor_idx = idx;
            compact_cursor = (idx + 1) % WAL_MAX_ENTRIES;
            break;
        }
    }
    if (!anchor) { compact_cursor = 0; return; }

    uint32_t latest_seq = 0;
    uint32_t latest_idx = 0;
    uint32_t dup_count = 0;

    for (uint32_t i = 0; i < WAL_MAX_ENTRIES; i++) {
        wal_entry_t *e = &g_wal->entries[i];
        if (e->seq != 0 && e->key_hash == anchor->key_hash &&
            (e->flags & WAL_ENTRY_ACTIVE) && !(e->flags & WAL_ENTRY_COMPACTED)) {
            dup_count++;
            if (e->seq > latest_seq) { latest_seq = e->seq; latest_idx = i; }
        }
    }
    if (dup_count <= 1) return;

    for (uint32_t i = 0; i < WAL_MAX_ENTRIES; i++) {
        if (i == latest_idx) continue;
        wal_entry_t *e = &g_wal->entries[i];
        if (e->seq != 0 && e->key_hash == anchor->key_hash &&
            (e->flags & WAL_ENTRY_ACTIVE) && !(e->flags & WAL_ENTRY_COMPACTED)) {
            g_wal->slot_free[e->slot] = 1;
            e->flags = WAL_ENTRY_COMPACTED;
            e->seq = 0;
            g_wal->entry_count--;
            g_wal->compactions++;
            g_wal->slots_reclaimed++;
        }
    }

    wal_entry_t *latest = &g_wal->entries[latest_idx];
    if (latest->flags & WAL_ENTRY_TOMBSTONE) {
        g_wal->slot_free[latest->slot] = 1;
        latest->flags = WAL_ENTRY_COMPACTED;
        latest->seq = 0;
        g_wal->entry_count--;
        g_wal->slots_reclaimed++;
    }
}

// ============================================================
// Core 1 Main Loop — pop forward FIFO for requests,
// push reverse FIFO for responses. Compact when idle.
// ============================================================

void wal_engine_run(wal_state_t *wal) {
    g_wal = wal;
    printf("[wal] Core 1 WAL engine ready (%u slots, %u req ring, bidirectional FIFO)\n",
           SLOT_COUNT, REQ_RING_SIZE);

    while (true) {
        // Drain all pending request signals (non-blocking check)
        bool did_work = false;

        while (multicore_fifo_rvalid()) {
            uint32_t word = multicore_fifo_pop_blocking();
            uint8_t req_id = fifo_req_id(word);

            wal_dmb();  // fence: ensure we see Core 0's writes to request ring

            wal_request_t *req = &g_wal->requests[req_id];
            did_work = true;

            switch (req->op) {
            case WAL_OP_APPEND:
                wal_do_append(req_id);
                break;
            case WAL_OP_READ:
                wal_do_read(req_id);
                break;
            case WAL_OP_NOOP:
            default:
                g_wal->responses[req_id].status = WAL_RESP_OK;
                wal_dmb();  // fence: commit response before ownership transfer
                req->ready = REQ_DONE;
                multicore_fifo_push_blocking(fifo_signal(req_id));
                break;
            }
        }

        // Background compaction when no FIFO work
        if (!did_work) {
            wal_compact_step();
        }

        tight_loop_contents();
    }
}
