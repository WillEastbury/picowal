#ifndef WAL_DEFS_H
#define WAL_DEFS_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Buffer Pool: 32 slots × 512B = 16KB SRAM (max card = 508 bytes)
// ============================================================

#define SLOT_COUNT  32
#define SLOT_SIZE   512

// ============================================================
// Request / Response Ring: 32 entries
//
// Ownership protocol (memory fence discipline):
//
//   ready=0 (EMPTY)   → Core 0 OWNS. Core 1 must not read/write.
//   ready=1 (PENDING) → Core 1 OWNS. Core 0 must not read/write.
//   ready=2 (DONE)    → Core 0 OWNS. Core 1 must not read/write.
//
// The `ready` field is the ownership fence. It must be written
// LAST after all other fields, with a __dmb() before the store
// to ensure all prior writes are visible to the other core.
//
// Core 0 flow: fill request fields → __dmb() → set ready=PENDING
// Core 1 flow: process → fill response → __dmb() → set ready=DONE
// Core 0 flow: read response → __dmb() → set ready=EMPTY (recycle)
//
// No forward FIFO. Reverse FIFO (Core 1 → Core 0) only signals
// req_id so Core 0 doesn't have to poll.
// ============================================================

#define REQ_RING_SIZE 32  // must be power of 2

// Request ops (stored in ring slot — 0 means empty/noop)
#define WAL_OP_NOOP   0
#define WAL_OP_APPEND 1
#define WAL_OP_READ   2
#define WAL_OP_KV_GET 3
#define WAL_OP_KV_PUT 4

typedef struct {
    volatile uint8_t  ready;     // ownership fence: 0=Core0, 1=Core1, 2=Core0
    uint8_t  op;                 // WAL_OP_*
    uint8_t  slot;               // buffer slot (APPEND: delta header written here)
    uint16_t len;                // total payload byte count
    uint32_t key_hash;           // key hash (READ / APPEND)

    // Zero-copy: pointer to value data (lives in pbuf or slot memory).
    // If zc_data != NULL, Core 1 reads directly from this pointer
    // instead of from slot data. Core 0 holds the pbuf ref until
    // Core 1 signals DONE.
    const uint8_t *zc_data;      // zero-copy value payload pointer (or NULL)
    uint16_t       zc_len;       // length of zero-copy data
    void          *zc_pbuf;      // opaque pbuf pointer for Core 0 to free
} wal_request_t;

#define REQ_EMPTY   0  // Core 0 owns
#define REQ_PENDING 1  // Core 1 owns
#define REQ_DONE    2  // Core 0 owns

// Response status
#define WAL_RESP_OK     0
#define WAL_RESP_ERR    1

typedef struct {
    uint8_t  status;      // WAL_RESP_*
    uint8_t  result_slot; // buffer slot with compacted result (READ)
    uint16_t result_len;  // total bytes in result slot (READ)
    uint32_t seq;         // assigned sequence number (APPEND ack)
    uint32_t delta_count; // number of deltas merged into result (READ)
} wal_response_t;

// ============================================================
// WAL Delta Metadata
// ============================================================

#define WAL_MAX_ENTRIES 32

typedef struct {
    uint32_t seq;
    uint32_t key_hash;
    uint8_t  slot;
    uint16_t len;
    uint8_t  flags;
} wal_entry_t;

#define WAL_ENTRY_ACTIVE    0x01
#define WAL_ENTRY_COMPACTED 0x02
#define WAL_ENTRY_TOMBSTONE 0x04

// ============================================================
// WAL State (shared between cores)
// ============================================================

typedef struct {
    // --- Buffer pool ---
    uint8_t data[SLOT_COUNT][SLOT_SIZE];
    volatile uint8_t slot_free[SLOT_COUNT];

    // --- WAL index ---
    wal_entry_t entries[WAL_MAX_ENTRIES];
    volatile uint32_t entry_count;
    volatile uint32_t next_seq;

    // --- Request ring (Core 0 writes, Core 1 reads) ---
    wal_request_t  requests[REQ_RING_SIZE];

    // --- Response ring (Core 1 writes, Core 0 reads) ---
    wal_response_t responses[REQ_RING_SIZE];

    // --- Compaction stats ---
    volatile uint32_t compactions;
    volatile uint32_t slots_reclaimed;

    // --- Request counters (Core 1 increments) ---
    volatile uint32_t req_appends;
    volatile uint32_t req_reads;
    volatile uint32_t req_total;

    // --- Liveness counters ---
    volatile uint32_t core0_heartbeat;
    volatile uint32_t core1_heartbeat;

    // --- OTA: set by Core 0 to park Core 1 during flash writes ---
    volatile bool ota_halt_core1;
} wal_state_t;

// ============================================================
// Bidirectional FIFO signaling (hardware, 8-deep each way)
//
// Core 0 → Core 1: push req_id when request is ready (PENDING)
// Core 1 → Core 0: push req_id when response is ready (DONE)
//
// Core 1 blocks on fifo_pop — wakes only when work arrives.
// When forward FIFO is empty, Core 1 runs background compaction.
// ============================================================

static inline uint32_t fifo_signal(uint8_t req_id) {
    return (uint32_t)req_id;
}

static inline uint8_t fifo_req_id(uint32_t word) {
    return (uint8_t)(word & 0xFF);
}

// Non-blocking FIFO push with bounded retry (avoids deadlock).
// Spins up to ~1ms before giving up. Returns true if pushed.
#include "pico/multicore.h"
#include "hardware/timer.h"
static inline bool fifo_push_timeout(uint32_t word) {
    uint64_t deadline = time_us_64() + 1000;
    while (!multicore_fifo_wready()) {
        if (time_us_64() > deadline) return false;
        tight_loop_contents();
    }
    multicore_fifo_push_blocking(word);  // guaranteed immediate — we checked wready
    return true;
}

// ============================================================
// Delta payload header (first bytes of each slot's data)
// ============================================================

typedef struct __attribute__((packed)) {
    uint32_t key_hash;
    uint16_t value_len;
    uint8_t  op;          // DELTA_OP_SET / DELTA_OP_DELETE
    uint8_t  reserved;
} delta_header_t;

#define DELTA_OP_SET    0
#define DELTA_OP_DELETE 1

#endif

