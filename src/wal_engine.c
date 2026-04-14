#include "wal_engine.h"
#include "wal_defs.h"
#include "wal_fence.h"
#include "kv_flash.h"

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

    // Copy zero-copy data into slot if needed
    if (req->zc_data && req->zc_len > 0) {
        memcpy(g_wal->data[req->slot] + sizeof(delta_header_t),
               req->zc_data, req->zc_len);
    }

    delta_header_t hdr;
    memcpy(&hdr, g_wal->data[req->slot], sizeof(hdr));

    // ---- Read-Merge-Write flow (never mutate in place) ----
    //
    // 1. Read current record from flash (via KV index)
    // 2. Merge incoming field changes into SRAM buffer
    // 3. Append merged record to new flash sector
    // 4. KV index atomically points key → new sector
    // 5. Old sector invalidated (dead, awaiting reclaim)

    if (hdr.op == DELTA_OP_DELETE) {
        // Delete: just remove from flash, no merge needed
        kv_delete(hdr.key_hash);
    } else {
        // Merge: read existing record, overlay incoming fields
        uint8_t merged[KV_MAX_VALUE];
        uint16_t merged_len = 0;

        // Read existing record from flash
        uint16_t existing_len = KV_MAX_VALUE;
        if (kv_get_copy(hdr.key_hash, merged, &existing_len, NULL) &&
            existing_len > 0 && existing_len <= KV_MAX_VALUE) {
            merged_len = existing_len;
        }

        // Parse incoming delta fields and overlay onto merged buffer
        // Delta payload format: [field_id:2][data_len:2][data...] × N
        const uint8_t *delta = g_wal->data[req->slot] + sizeof(delta_header_t);
        uint16_t delta_len = hdr.value_len;
        uint16_t dpos = 0;

        while (dpos + 4 <= delta_len) {
            uint16_t field_id, data_len;
            memcpy(&field_id, delta + dpos, 2);
            memcpy(&data_len, delta + dpos + 2, 2);
            dpos += 4;

            if (dpos + data_len > delta_len) break;

            // Find this field in the merged buffer and replace it,
            // or append if not found
            uint16_t mpos = 0;
            bool replaced = false;

            while (mpos + 4 <= merged_len) {
                uint16_t mf_id, mf_len;
                memcpy(&mf_id, merged + mpos, 2);
                memcpy(&mf_len, merged + mpos + 2, 2);

                if (mf_id == field_id) {
                    // Replace: shift tail, insert new data
                    uint16_t old_entry_size = 4 + mf_len;
                    uint16_t new_entry_size = 4 + data_len;
                    int16_t  size_diff = (int16_t)new_entry_size - (int16_t)old_entry_size;
                    uint16_t tail_start = mpos + old_entry_size;
                    uint16_t tail_len = merged_len - tail_start;

                    if (merged_len + size_diff > KV_MAX_VALUE) break;

                    // Shift tail
                    if (tail_len > 0 && size_diff != 0)
                        memmove(merged + mpos + new_entry_size,
                                merged + tail_start, tail_len);

                    // Write new field header + data
                    memcpy(merged + mpos, &field_id, 2);
                    memcpy(merged + mpos + 2, &data_len, 2);
                    memcpy(merged + mpos + 4, delta + dpos, data_len);

                    merged_len = (uint16_t)((int16_t)merged_len + size_diff);
                    replaced = true;
                    break;
                }

                mpos += 4 + mf_len;
            }

            if (!replaced) {
                // Append new field
                if (merged_len + 4 + data_len <= KV_MAX_VALUE) {
                    memcpy(merged + merged_len, &field_id, 2);
                    memcpy(merged + merged_len + 2, &data_len, 2);
                    memcpy(merged + merged_len + 4, delta + dpos, data_len);
                    merged_len += 4 + data_len;
                }
            }

            dpos += data_len;
        }

        // Write merged record to flash (append-only, new sector)
        kv_put(hdr.key_hash, merged, merged_len);
    }

    // Register in SRAM WAL index
    int idx = wal_alloc_entry();
    if (idx < 0) {
        // WAL index full but flash write succeeded — still OK
        g_wal->slot_free[req->slot] = 1;
        resp->status = WAL_RESP_OK;
        resp->seq    = g_wal->next_seq++;
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

static void wal_do_kv_get(uint8_t req_id) {
    wal_request_t *req = &g_wal->requests[req_id];
    wal_response_t *resp = &g_wal->responses[req_id];
    memset(resp, 0, sizeof(*resp));

    int rslot = wal_alloc_slot();
    if (rslot < 0) {
        resp->status = WAL_RESP_ERR;
        wal_dmb();
        req->ready = REQ_DONE;
        multicore_fifo_push_blocking(fifo_signal(req_id));
        return;
    }

    uint16_t len = SLOT_SIZE;
    if (!kv_get_copy(req->key_hash, g_wal->data[rslot], &len, NULL)) {
        g_wal->slot_free[rslot] = 1;
        resp->status = WAL_RESP_ERR;
    } else {
        g_wal->slot_free[rslot] = 0;
        resp->status = WAL_RESP_OK;
        resp->result_slot = (uint8_t)rslot;
        resp->result_len = len;
    }

    wal_dmb();
    req->ready = REQ_DONE;
    multicore_fifo_push_blocking(fifo_signal(req_id));
}

static void wal_do_kv_put(uint8_t req_id) {
    wal_request_t *req = &g_wal->requests[req_id];
    wal_response_t *resp = &g_wal->responses[req_id];
    memset(resp, 0, sizeof(*resp));

    resp->status = kv_put(req->key_hash, g_wal->data[req->slot], req->len) ? WAL_RESP_OK : WAL_RESP_ERR;
    g_wal->slot_free[req->slot] = 1;

    wal_dmb();
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
        // OTA halt: spin here until Core 0 finishes flash writes
        while (g_wal->ota_halt_core1) {
            __wfe();  // wait-for-event, low power
        }

        g_wal->core1_heartbeat++;

        // Drain all pending request signals (non-blocking check)
        bool did_work = false;

        while (multicore_fifo_rvalid()) {
            uint32_t word = multicore_fifo_pop_blocking();
            uint8_t req_id = fifo_req_id(word);

            if (req_id >= REQ_RING_SIZE) continue;  // bounds check

            wal_dmb();// fence: ensure we see Core 0's writes to request ring

            wal_request_t *req = &g_wal->requests[req_id];
            did_work = true;

            switch (req->op) {
            case WAL_OP_APPEND:
                wal_do_append(req_id);
                g_wal->req_appends++;
                g_wal->req_total++;
                break;
            case WAL_OP_READ:
                wal_do_read(req_id);
                g_wal->req_reads++;
                g_wal->req_total++;
                break;
            case WAL_OP_KV_GET:
                wal_do_kv_get(req_id);
                g_wal->req_reads++;
                g_wal->req_total++;
                break;
            case WAL_OP_KV_PUT:
                wal_do_kv_put(req_id);
                g_wal->req_appends++;
                g_wal->req_total++;
                break;
            case WAL_OP_NOOP:
            default:
                g_wal->responses[req_id].status = WAL_RESP_OK;
                wal_dmb();  // fence: commit response before ownership transfer
                req->ready = REQ_DONE;
                multicore_fifo_push_blocking(fifo_signal(req_id));
                g_wal->req_total++;
                break;
            }
        }

        // Background compaction when no FIFO work
        if (!did_work) {
            wal_compact_step();
            kv_compact_step();
        }

        tight_loop_contents();
    }
}
