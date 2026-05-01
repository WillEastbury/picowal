#ifndef PICOCLUSTER_DISCOVERY_H
#define PICOCLUSTER_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// DHCP-style address assignment protocol
// ============================================================
//
// Each RP2350 has a unique 64-bit flash ID (like a MAC address).
// Nodes boot unaddressed and request an ID from the head node.
//
// Protocol:
//   1. Node sends ADDR_REQ with its 64-bit hardware ID + role
//   2. Head receives REQ, assigns next sequential ID
//   3. Head sends ADDR_ACK with {hw_id, assigned_id}
//   4. Node sees ACK matching its hw_id, adopts the assigned_id
//   5. Node forwards all ADDR_REQ/ACK packets (ring relay)
//
// Nodes retry REQ every DISC_REQ_INTERVAL_MS until ACK received.
// Head maintains lease table mapping hw_id → assigned_id.
// ============================================================

#define MAX_CLUSTER_NODES    16
#define DISC_REQ_INTERVAL_MS 100
#define DISC_TIMEOUT_MS      3000
#define DISC_SETTLE_MS       500   // Wait after last ACK before done

// Hardware ID (from pico_get_unique_board_id)
typedef struct {
    uint8_t bytes[8];
} hw_id_t;

// ADDR_REQ payload (node → head): "I exist, give me an address"
typedef struct __attribute__((packed)) {
    hw_id_t hw_id;       // 8-byte unique board ID
    uint8_t role;        // ROLE_WORKER, ROLE_STORAGE, etc.
    uint8_t flags;       // Reserved
} addr_req_t;

// ADDR_ACK payload (head → node): "You are node N"
typedef struct __attribute__((packed)) {
    hw_id_t hw_id;       // Echo back the requester's hw_id
    uint8_t assigned_id; // 1-based node ID
    uint8_t flags;       // Reserved
} addr_ack_t;

// Lease table entry (head maintains this)
typedef struct {
    hw_id_t hw_id;
    uint8_t node_id;
    uint8_t role;
    bool    active;
} lease_entry_t;

// Lease table (head only)
typedef struct {
    lease_entry_t entries[MAX_CLUSTER_NODES];
    uint8_t       count;
    uint8_t       next_id;  // Next ID to assign (starts at 1)
} lease_table_t;

// --- Head API ---

// Initialize lease table and begin accepting REQs
void disc_master_init(lease_table_t *table);

// Process incoming packets (call from head's ring poll loop)
// Returns true if a REQ was handled (ACK sent)
bool disc_master_handle(lease_table_t *table, uint8_t ring,
                        const void *hdr, const uint8_t *payload);

// Check if discovery is settled (no new REQs for DISC_SETTLE_MS)
bool disc_master_settled(const lease_table_t *table);

// --- Participant API ---

// Send ADDR_REQ and wait for ACK. Returns assigned ID (1-254),
// or 0 on timeout (head not found).
uint8_t disc_participant_run(uint8_t my_role);

// Get this node's hardware ID
void disc_get_hw_id(hw_id_t *out);

#endif // PICOCLUSTER_DISCOVERY_H
