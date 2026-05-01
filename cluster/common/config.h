#ifndef PICOCLUSTER_CONFIG_H
#define PICOCLUSTER_CONFIG_H

// ============================================================
// PicoCluster — Hardware configuration & memory map
// ============================================================

#include <stdint.h>

// --- Clock ---
#define SYS_CLOCK_KHZ       450000   // 450 MHz overclock

// --- Node limits ---
#define MAX_NODES           16
#define MAX_RINGS           4

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

// --- PIO Ring pin assignments ---
// 4 rings × 2 pins (TX + RX) = 8 GPIOs
#define RING0_PIN_TX        0    // Express 1
#define RING0_PIN_RX        1
#define RING1_PIN_TX        2    // Express 2
#define RING1_PIN_RX        3
#define RING2_PIN_TX        4    // Normal
#define RING2_PIN_RX        5
#define RING3_PIN_TX        6    // Storage
#define RING3_PIN_RX        7

// --- Ring configuration ---
#define RING_BAUD_RATE      20000000  // 20 Mbps Manchester (10 MHz symbol rate)
#define RING_PIO_BLOCK_0    0         // Rings 0,1 on PIO0
#define RING_PIO_BLOCK_1    1         // Rings 2,3 on PIO1

// --- DMA channel allocation ---
// Rings: 0-7 (4 rings × 2 channels each)
#define DMA_CH_RING0_TX     0
#define DMA_CH_RING0_RX     1
#define DMA_CH_RING1_TX     2
#define DMA_CH_RING1_RX     3
#define DMA_CH_RING2_TX     4
#define DMA_CH_RING2_RX     5
#define DMA_CH_RING3_TX     6
#define DMA_CH_RING3_RX     7
// General purpose: 8-11
#define DMA_CH_GENERAL_0    8
#define DMA_CH_GENERAL_1    9
#define DMA_CH_SD           10
#define DMA_CH_W5500        11

// --- Memory map (520KB SRAM) ---
#define MEM_VM_DATA_SIZE    (128 * 1024)   // 128KB VM working memory
#define MEM_VM_STACK_SIZE   (32 * 1024)    // 32KB VM call/data stack
#define MEM_CARD_CACHE_SIZE (128 * 1024)   // 128KB card cache (SRAM)
#define MEM_RING_BUF_SIZE   (32 * 1024)    // 32KB ring packet buffers
#define MEM_RESULT_BUF_SIZE (16 * 1024)    // 16KB result assembly buffer
#define MEM_SCRATCH_SIZE    (32 * 1024)    // 32KB scratch/heap
// Total: 368KB allocated, ~152KB headroom

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
    uint16_t flags;
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
