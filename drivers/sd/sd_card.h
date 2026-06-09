#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>
#include <stdbool.h>

// Waveshare Pico-ResTouch-LCD-3.5 SD card slot
// Empirically verified: hardware SPI0 worked with SCK=GP18, TX=GP19, RX=GP20.
// The board's SDIO labels don't match SPI pin roles:
//   GP18 (SDIO_CMD) → actually wired to SD CLK
//   GP19 (D0)       → actually wired to SD CMD/MOSI  
//   GP20 (D1)       → actually wired to SD DAT0/MISO
//   GP22 (D3/CS)    → SD CS
// Using PIO since GP18 as SCK can't be done on hardware SPI0 (it's SPI0_SCK though!)
// Actually GP18 IS SPI0_SCK — so hardware SPI0 should work too. Let's use PIO anyway.
// Waveshare Pico-ResTouch-LCD-3.5 SD card slot
// SD card shares SPI1 with the LCD (GP10=SCK, GP11=MOSI, GP12=MISO)
// Only CS is different: GP22 for SD, GP9 for LCD, GP16 for touch
#define SD_SPI_PORT   spi1
#define SD_CLK_PIN    10    // SPI1 SCK (shared with LCD)
#define SD_MOSI_PIN   11    // SPI1 MOSI (shared with LCD)
#define SD_MISO_PIN   12    // SPI1 MISO (shared with LCD)
#define SD_CS_PIN     22    // SD CS (D3)

#define SD_BLOCK_SIZE 512

bool sd_init(void);
bool sd_read_block(uint32_t block_addr, uint8_t *buf);
bool sd_write_block(uint32_t block_addr, const uint8_t *buf);
bool sd_read_blocks(uint32_t block_addr, uint8_t *buf, uint32_t count);
bool sd_write_blocks(uint32_t block_addr, const uint8_t *buf, uint32_t count);

typedef struct {
    uint32_t block_count;
    uint32_t capacity_mb;
    bool     sdhc;
} sd_info_t;

bool sd_get_info(sd_info_t *info);
const char *sd_get_debug(void);

#endif
