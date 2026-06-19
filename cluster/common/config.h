#ifndef PICOCLUSTER_CONFIG_H
#define PICOCLUSTER_CONFIG_H

// ============================================================
// PicoCluster — Hardware configuration & memory map
// Tree topology: Head → Workers ← Storage (cross-connected)
// All links half-duplex single-wire Manchester via PIO
// ============================================================

#include <stdint.h>

// --- Clock ---
#define SYS_CLOCK_KHZ       300000   // 300 MHz (safe overclock)
#define SYS_CLOCK_IDLE_KHZ  12000   // 12 MHz idle (5 mA)
#define SYS_CLOCK_LOW_KHZ   48000   // 48 MHz light work
#define SYS_CLOCK_MID_KHZ  150000   // 150 MHz moderate
#define SYS_CLOCK_MAX_KHZ  300000   // 300 MHz full blast

// --- Node limits ---
#define MAX_NODES           16
#define MAX_WORKERS         12
#define MAX_STORAGE         3
#define WORKERS_PER_STORAGE 4

// --- Topology ---
// Head: 12 half-duplex ports → 12 workers (command/result path)
// Storage: 4 worker ports + 2 cross-connects to other storage nodes
// Worker: 2 half-duplex links (1 to head, 1 to storage)
//
//   HEAD ──────── 12 workers (command/result)
//                  │ │ │ │ │ │ │ │ │ │ │ │
//                  └─┴─┴─┘ └─┴─┴─┘ └─┴─┴─┘
//                    S1       S2       S3  (card data path)
//                    ├────────┼────────┤   (cross-connects)
//
// PIO usage:
//   Head:    12 SMs (1 per worker, half-duplex)
//   Worker:   2 SMs (1 head link + 1 storage link)
//   Storage:  6 SMs (4 worker + 2 cross-connect), SPI1 for SD

// --- Link configuration ---
#define LINK_BAUD_RATE      50000000  // 50 Mbps Manchester half-duplex
#define LINK_TURNAROUND_US  2         // Direction switch gap

// --- Worker pin assignments (2 GPIOs) ---
#define WORKER_HEAD_PIN     0    // Half-duplex link to head
#define WORKER_STORE_PIN    1    // Half-duplex link to storage

// --- Head pin assignments (12 GPIOs + SPI0) ---
#define HEAD_PORT_BASE      0    // GP0-GP11: 12 worker ports
#define HEAD_PORT_COUNT     12
// SPI0: W5500 Ethernet
#define W5500_SPI_INST      spi0
#define W5500_SPI_BAUD      40000000
#define W5500_PIN_MISO      16
#define W5500_PIN_MOSI      19
#define W5500_PIN_SCK       18
#define W5500_PIN_CS        17
#define W5500_PIN_RST       20
#define W5500_PIN_INT       21

// --- Storage pin assignments (6 GPIOs + SPI1) ---
#define STORE_WORKER_BASE   0    // GP0-GP3: 4 worker ports
#define STORE_WORKER_COUNT  4
#define STORE_XCONN_BASE    4    // GP4-GP5: cross-connects to other storage
#define STORE_XCONN_COUNT   2
// SPI1: SD Card
#define SD_SPI_INST         spi1
#define SD_SPI_BAUD         25000000
#define SD_PIN_MISO         12
#define SD_PIN_MOSI         15
#define SD_PIN_SCK          14
#define SD_PIN_CS           13

// --- DMA channel allocation ---
// Half-duplex: 1 DMA channel per port (shared TX/RX)
// Head: channels 0-11 (12 ports)
// Worker: channels 0-1 (head + storage)
// Storage: channels 0-5 (4 worker + 2 xconn)
#define DMA_CH_GENERAL      14
#define DMA_CH_SPI          15

// --- Memory map (520KB SRAM) ---
#define MEM_VM_DATA_SIZE    (128 * 1024)   // 128KB VM working memory
#define MEM_VM_STACK_SIZE   (32 * 1024)    // 32KB VM call/data stack
#define MEM_CARD_CACHE_SIZE (128 * 1024)   // 128KB card cache (SRAM)
#define MEM_LINK_BUF_SIZE   (16 * 1024)    // 16KB link buffers
#define MEM_RESULT_BUF_SIZE (16 * 1024)    // 16KB result assembly
#define MEM_SCRATCH_SIZE    (32 * 1024)    // 32KB scratch/heap
// Total: 352KB, ~168KB headroom

// --- Flash layout (4MB) ---
#define FLASH_FIRMWARE_BASE  0x10000000
#define FLASH_FIRMWARE_SIZE  (256 * 1024)
#define FLASH_CONFIG_BASE    (FLASH_FIRMWARE_BASE + FLASH_FIRMWARE_SIZE)
#define FLASH_CONFIG_SIZE    (4 * 1024)
#define FLASH_CARD_INDEX     (FLASH_CONFIG_BASE + FLASH_CONFIG_SIZE)
#define FLASH_CARD_INDEX_SIZE (4 * 1024)
#define FLASH_CARD_DATA      (FLASH_CARD_INDEX + FLASH_CARD_INDEX_SIZE)
#define FLASH_CARD_DATA_END  (FLASH_FIRMWARE_BASE + (4 * 1024 * 1024))

// --- Flash config ---
typedef struct {
    uint32_t magic;            // 0x50434C53 = "PCLS"
    uint8_t  node_id;          // Assigned node ID (0xFF = unassigned)
    uint8_t  role_override;    // 0=auto, 1=head, 2=storage, 3=worker
    uint8_t  storage_group;    // Which storage node this worker uses (0-2)
    uint8_t  port_index;       // Port index on head (0-11) or storage (0-3)
    uint32_t card_count;
    uint32_t boot_count;
    uint8_t  reserved[44];
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

// --- Special addresses ---
#define ADDR_BROADCAST       0xFF
#define ADDR_MASTER          0x00
#define ADDR_STORAGE_BASE    0xF0   // Storage nodes: 0xF0, 0xF1, 0xF2

#endif // PICOCLUSTER_CONFIG_H
