#ifndef PICOCLUSTER_SD_H
#define PICOCLUSTER_SD_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// SD Card driver (SPI mode, raw sector I/O)
// ============================================================

#define SD_BLOCK_SIZE  512

typedef enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_V1,      // SD v1
    SD_TYPE_V2,      // SD v2 (standard capacity)
    SD_TYPE_SDHC,    // SDHC/SDXC (block-addressed)
} sd_type_t;

typedef struct {
    sd_type_t type;
    uint32_t  sector_count;   // Total sectors
    uint32_t  capacity_mb;    // Capacity in MB
    bool      initialized;
} sd_info_t;

// Initialise SD card (SPI already configured by detect phase)
// Returns true if card is ready
bool sd_init(sd_info_t *info);

// Read a single 512-byte sector
bool sd_read_sector(uint32_t sector, uint8_t *buf);

// Write a single 512-byte sector
bool sd_write_sector(uint32_t sector, const uint8_t *buf);

// Read multiple sectors (DMA-friendly)
bool sd_read_sectors(uint32_t start_sector, uint32_t count, uint8_t *buf);

// Write multiple sectors
bool sd_write_sectors(uint32_t start_sector, uint32_t count, const uint8_t *buf);

// Get card info
void sd_get_info(sd_info_t *info);

// Check if card is present and responding
bool sd_is_ready(void);

#endif // PICOCLUSTER_SD_H
