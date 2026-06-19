#ifndef PICOCLUSTER_DETECT_H
#define PICOCLUSTER_DETECT_H

#include <stdint.h>

// Node roles determined by hardware detection
typedef enum {
    ROLE_UNKNOWN = 0,
    ROLE_HEAD,       // W5500 detected — network gateway
    ROLE_STORAGE,    // SD card detected (no W5500) — card server
    ROLE_WORKER,     // Neither — pure compute
} node_role_t;

// Probe hardware and determine role
// Call once at boot after clock init
node_role_t detect_role(void);

// Individual detection results (for diagnostics)
typedef struct {
    bool     w5500_present;
    uint8_t  w5500_version;     // Chip version register
    bool     sd_present;
    uint8_t  sd_type;           // 0=none, 1=SDv1, 2=SDv2, 3=SDHC
    bool     wifi_present;      // CYW43 responds (Pico2W only)
} detect_result_t;

// Detailed probe (fills all fields)
void detect_probe_all(detect_result_t *result);

#endif // PICOCLUSTER_DETECT_H
