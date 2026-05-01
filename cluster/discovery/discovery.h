#ifndef PICOCLUSTER_DISCOVERY_H
#define PICOCLUSTER_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// Ring topology discovery protocol
// ============================================================

// Discovery states
typedef enum {
    DISC_IDLE = 0,
    DISC_ANNOUNCING,
    DISC_LEARNING,
    DISC_COMPLETE,
} discovery_state_t;

// Node info learned during discovery
typedef struct {
    uint8_t  node_id;
    uint8_t  role;          // ROLE_HEAD, ROLE_STORAGE, ROLE_WORKER
    uint8_t  ring_position; // Hop count from master
    uint8_t  flags;
} discovered_node_t;

#define MAX_DISCOVERED_NODES 16

// Discovery result
typedef struct {
    discovered_node_t nodes[MAX_DISCOVERED_NODES];
    uint8_t           node_count;
    uint8_t           my_position;     // My hop distance from head
    uint8_t           assigned_id;     // ID assigned by head
    discovery_state_t state;
} discovery_result_t;

// Run discovery as master (head node)
// Sends DISCOVER broadcast, collects responses, assigns IDs
void discovery_run_master(void);

// Run discovery as participant (worker/storage)
// Listens for DISCOVER, responds, waits for ID assignment
void discovery_run_participant(discovery_result_t *result);

// Get assigned node ID (valid after discovery completes)
uint8_t discovery_get_id(void);

#endif // PICOCLUSTER_DISCOVERY_H
