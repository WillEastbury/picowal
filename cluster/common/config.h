#ifndef PICOCLUSTER_CONFIG_H
#define PICOCLUSTER_CONFIG_H

// ============================================================
// PicoCluster — Hardware configuration & memory map
// Star topology with PIO switching fabric on head nodes
// ============================================================

#include <stdint.h>

// --- Clock ---
#define SYS_CLOCK_KHZ       450000   // 450 MHz overclock

// --- Node limits ---
#define MAX_NODES           16
#define PORTS_PER_HEAD      6        // 6 nodes per head (uses all 12 PIO SMs)
#define NUM_HEADS           2        // 2 head nodes form the fabric

// --- Topology: Star/Snowflake ---
// Head nodes act as central switches using PIO as crossbar.
// Workers connect point-to-point to their head (2 wires: TX + RX).
// Head-to-head link for cross-fabric traffic (1 extra pair).
//
// Head PIO allocation (12 SMs total = 3 blocks × 4):
//   PIO0: ports 0-1 (4 SMs: 2 TX + 2 RX)
//   PIO1: ports 2-3 (4 SMs: 2 TX + 2 RX)
//   PIO2: ports 4-5 (4 SMs: 2 TX + 2 RX)
//   -- OR head-to-head link uses 2 SMs from PIO2, leaving 5 worker ports

// --- Worker/Storage pin assignments (simple: 2 GPIOs) ---
#define LINK_PIN_TX         0    // TX to head
#define LINK_PIN_RX         1    // RX from head

// --- Head node: port pin assignments ---
// 6 ports × 2 pins = 12 GPIOs (GP0-GP11)
#define HEAD_PORT0_TX       0
#define HEAD_PORT0_RX       1
#define HEAD_PORT1_TX       2
#define HEAD_PORT1_RX       3
#define HEAD_PORT2_TX       4
#define HEAD_PORT2_RX       5
#define HEAD_PORT3_TX       6
#define HEAD_PORT3_RX       7
#define HEAD_PORT4_TX       8
#define HEAD_PORT4_RX       9
#define HEAD_PORT5_TX       10
#define HEAD_PORT5_RX       11

// --- Head-to-head interlink (uses GP26-GP27) ---
#define HEAD_INTERLINK_TX   26
#define HEAD_INTERLINK_RX   27

// --- SPI0: W5500 Ethernet (Head node detection) ---
#define W5500_SPI_INST      spi0
#define W5500_SPI_BAUD      40000000  // 40 MHz
#define W5500_PIN_MISO      16
#define W5500_PIN_MOSI      19
#define W5500_PIN_SCK       18
#define W5500_PIN_CS        17
#define W5500_PIN_RST       20
#define W5500_PIN_INT       21

// --- SPI1: SD Card (Storage node detection) ---
#define SD_SPI_INST         spi1
#define SD_SPI_BAUD         25000000  // 25 MHz
#define SD_PIN_MISO         12
#define SD_PIN_MOSI         15
#define SD_PIN_SCK          14
#define SD_PIN_CS           13

// --- Link configuration ---
#define LINK_BAUD_RATE      20000000  // 20 Mbps Manchester (10 MHz symbol rate)

// --- DMA channel allocation ---
// Head: 12 channels for 6 ports (TX + RX each)
#define DMA_CH_PORT0_TX     0
#define DMA_CH_PORT0_RX     1
#define DMA_CH_PORT1_TX     2
#define DMA_CH_PORT1_RX     3
#define DMA_CH_PORT2_TX     4
#define DMA_CH_PORT2_RX     5
#define DMA_CH_PORT3_TX     6
#define DMA_CH_PORT3_RX     7
#define DMA_CH_PORT4_TX     8
#define DMA_CH_PORT4_RX     9
#define DMA_CH_PORT5_TX     10
#define DMA_CH_PORT5_RX     11
// Worker: only 2 channels needed
#define DMA_CH_LINK_TX      0
#define DMA_CH_LINK_RX      1
// General purpose
#define DMA_CH_GENERAL_0    12
#define DMA_CH_SD           13
#define DMA_CH_W5500        14

// --- Memory map (520KB SRAM) ---
#define MEM_VM_DATA_SIZE    (128 * 1024)   // 128KB VM working memory
#define MEM_VM_STACK_SIZE   (32 * 1024)    // 32KB VM call/data stack
#define MEM_CARD_CACHE_SIZE (128 * 1024)   // 128KB card cache (SRAM)
#define MEM_LINK_BUF_SIZE   (16 * 1024)    // 16KB link buffers (worker: 2×4KB, head: 6×2KB+routing)
#define MEM_RESULT_BUF_SIZE (16 * 1024)    // 16KB result assembly buffer
#define MEM_SCRATCH_SIZE    (32 * 1024)    // 32KB scratch/heap
// Head extra: switch buffer pool
#define MEM_SWITCH_BUF_SIZE (48 * 1024)    // 48KB packet switch buffers (6 ports × 8KB)
// Total worker: 352KB allocated, ~168KB headroom
// Total head:   400KB allocated, ~120KB headroom

// --- Flash layout (4MB) ---
#define FLASH_FIRMWARE_BASE  0x10000000
#define FLASH_FIRMWARE_SIZE  (256 * 1024)
#define FLASH_CONFIG_BASE    (FLASH_FIRMWARE_BASE + FLASH_FIRMWARE_SIZE)
#define FLASH_CONFIG_SIZE    (4 * 1024)    // 4KB node config (ID, role override)
#define FLASH_CARD_INDEX     (FLASH_CONFIG_BASE + FLASH_CONFIG_SIZE)
#define FLASH_CARD_INDEX_SIZE (4 * 1024)   // 4KB card index
#define FLASH_CARD_DATA      (FLASH_CARD_INDEX + FLASH_CARD_INDEX_SIZE)
#define FLASH_CARD_DATA_END  (FLASH_FIRMWARE_BASE + (4 * 1024 * 1024))
// Usable card storage: ~3.73 MB

// --- Flash config structure (persisted) ---
typedef struct {
    uint32_t magic;            // 0x50434C53 = "PCLS"
    uint8_t  node_id;          // Assigned node ID (0xFF = unassigned)
    uint8_t  role_override;    // 0 = auto-detect, 1=head, 2=storage, 3=worker
    uint8_t  head_id;          // Which head this node connects to (0 or 1)
    uint8_t  port_on_head;     // Which port on the head (0-5)
    uint32_t card_count;       // Number of cards in flash
    uint32_t boot_count;
    uint8_t  reserved[44];     // Pad to 64 bytes
} flash_config_t;

#define FLASH_CONFIG_MAGIC   0x50434C53

// --- Packet limits ---
#define PKT_MAX_PAYLOAD      4096
#define PKT_HEADER_SIZE      8

// --- VM execution limits ---
#define VM_MAX_CYCLES_PER_RUN  100000
#define VM_EXEC_QUEUE_SIZE     8

// --- Timing ---
#define HEARTBEAT_INTERVAL_MS  1000
#define DISCOVERY_TIMEOUT_MS   5000
#define WATCHDOG_TIMEOUT_MS    8000

#endif // PICOCLUSTER_CONFIG_H
