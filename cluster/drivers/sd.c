#include "sd.h"
#include "../common/config.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include <string.h>

// ============================================================
// SD SPI low-level helpers
// ============================================================

static sd_info_t g_sd_info = {0};

static inline void sd_cs_low(void)  { gpio_put(SD_PIN_CS, 0); }
static inline void sd_cs_high(void) { gpio_put(SD_PIN_CS, 1); }

static uint8_t sd_spi_xfer(uint8_t tx) {
    uint8_t rx;
    spi_write_read_blocking(SD_SPI_INST, &tx, &rx, 1);
    return rx;
}

static void sd_spi_write(const uint8_t *data, uint32_t len) {
    spi_write_blocking(SD_SPI_INST, data, len);
}

static void sd_spi_read(uint8_t *data, uint32_t len) {
    spi_read_blocking(SD_SPI_INST, 0xFF, data, len);
}

// Wait for card not busy
static bool sd_wait_ready(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    while (time_us_64() < deadline) {
        if (sd_spi_xfer(0xFF) == 0xFF) return true;
    }
    return false;
}

// Send SD command, return R1 response
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];
    frame[0] = 0x40 | cmd;
    frame[1] = (arg >> 24) & 0xFF;
    frame[2] = (arg >> 16) & 0xFF;
    frame[3] = (arg >> 8) & 0xFF;
    frame[4] = arg & 0xFF;

    // CRC (required for CMD0 and CMD8, dummy for others)
    if (cmd == 0) frame[5] = 0x95;
    else if (cmd == 8) frame[5] = 0x87;
    else frame[5] = 0x01;

    sd_cs_low();
    sd_wait_ready(100);
    sd_spi_write(frame, 6);

    // Wait for response (bit 7 = 0)
    uint8_t r1;
    for (int i = 0; i < 16; i++) {
        r1 = sd_spi_xfer(0xFF);
        if ((r1 & 0x80) == 0) return r1;
    }
    return 0xFF;  // Timeout
}

static uint8_t sd_acmd(uint8_t cmd, uint32_t arg) {
    sd_cmd(55, 0);  // APP_CMD prefix
    sd_cs_high();
    sd_spi_xfer(0xFF);
    return sd_cmd(cmd, arg);
}

// ============================================================
// Initialisation
// ============================================================

bool sd_init(sd_info_t *info) {
    // SPI already configured by detect at 25 MHz
    // But re-init at low speed for card init
    spi_set_baudrate(SD_SPI_INST, 400000);

    // 80+ clock pulses with CS high
    sd_cs_high();
    for (int i = 0; i < 10; i++) sd_spi_xfer(0xFF);

    // CMD0: Reset
    uint8_t r1 = sd_cmd(0, 0);
    sd_cs_high();
    sd_spi_xfer(0xFF);

    if (r1 != 0x01) {
        if (info) { info->type = SD_TYPE_NONE; info->initialized = false; }
        return false;
    }

    // CMD8: Voltage check
    r1 = sd_cmd(8, 0x000001AA);
    sd_type_t type = SD_TYPE_V1;

    if (r1 == 0x01) {
        // SDv2 — read R7 response
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_spi_xfer(0xFF);
        sd_cs_high();
        sd_spi_xfer(0xFF);

        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            type = SD_TYPE_SDHC;  // Tentative — confirm with ACMD41
        } else {
            type = SD_TYPE_V2;
        }
    } else {
        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    // ACMD41: Initialisation (with HCS bit for SDHC)
    uint32_t acmd41_arg = (type == SD_TYPE_SDHC) ? 0x40000000 : 0;
    uint64_t deadline = time_us_64() + 2000000;  // 2 second timeout

    while (time_us_64() < deadline) {
        r1 = sd_acmd(41, acmd41_arg);
        sd_cs_high();
        sd_spi_xfer(0xFF);
        if (r1 == 0x00) break;  // Ready
        sleep_ms(10);
    }

    if (r1 != 0x00) {
        if (info) { info->type = SD_TYPE_NONE; info->initialized = false; }
        return false;
    }

    // CMD58: Read OCR to confirm SDHC
    if (type == SD_TYPE_SDHC) {
        r1 = sd_cmd(58, 0);
        if (r1 == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = sd_spi_xfer(0xFF);
            if (!(ocr[0] & 0x40)) {
                type = SD_TYPE_V2;  // Not SDHC after all
            }
        }
        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    // Set block size to 512 for non-SDHC cards
    if (type != SD_TYPE_SDHC) {
        sd_cmd(16, 512);
        sd_cs_high();
        sd_spi_xfer(0xFF);
    }

    // Bump SPI to full speed
    spi_set_baudrate(SD_SPI_INST, SD_SPI_BAUD);

    g_sd_info.type = type;
    g_sd_info.initialized = true;
    // TODO: read CSD to get sector_count/capacity

    if (info) *info = g_sd_info;
    return true;
}

// ============================================================
// Sector read/write
// ============================================================

bool sd_read_sector(uint32_t sector, uint8_t *buf) {
    if (!g_sd_info.initialized) return false;

    // SDHC uses block addressing, others use byte addressing
    uint32_t addr = (g_sd_info.type == SD_TYPE_SDHC) ? sector : sector * 512;

    uint8_t r1 = sd_cmd(17, addr);  // CMD17: READ_SINGLE_BLOCK
    if (r1 != 0x00) {
        sd_cs_high();
        return false;
    }

    // Wait for data token (0xFE)
    uint64_t deadline = time_us_64() + 200000;  // 200 ms
    while (time_us_64() < deadline) {
        uint8_t token = sd_spi_xfer(0xFF);
        if (token == 0xFE) {
            // Read 512 bytes + 2 CRC bytes
            sd_spi_read(buf, 512);
            sd_spi_xfer(0xFF);  // CRC hi
            sd_spi_xfer(0xFF);  // CRC lo
            sd_cs_high();
            sd_spi_xfer(0xFF);
            return true;
        }
        if (token != 0xFF) break;  // Error token
    }

    sd_cs_high();
    return false;
}

bool sd_write_sector(uint32_t sector, const uint8_t *buf) {
    if (!g_sd_info.initialized) return false;

    uint32_t addr = (g_sd_info.type == SD_TYPE_SDHC) ? sector : sector * 512;

    uint8_t r1 = sd_cmd(24, addr);  // CMD24: WRITE_BLOCK
    if (r1 != 0x00) {
        sd_cs_high();
        return false;
    }

    // Send data token
    sd_spi_xfer(0xFF);   // Gap
    sd_spi_xfer(0xFE);   // Data token

    // Write 512 bytes
    sd_spi_write(buf, 512);

    // Dummy CRC
    sd_spi_xfer(0xFF);
    sd_spi_xfer(0xFF);

    // Check data response
    uint8_t resp = sd_spi_xfer(0xFF);
    if ((resp & 0x1F) != 0x05) {
        sd_cs_high();
        return false;  // Write rejected
    }

    // Wait for card to finish programming
    if (!sd_wait_ready(500)) {
        sd_cs_high();
        return false;
    }

    sd_cs_high();
    sd_spi_xfer(0xFF);
    return true;
}

bool sd_read_sectors(uint32_t start_sector, uint32_t count, uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++) {
        if (!sd_read_sector(start_sector + i, buf + i * 512)) return false;
    }
    return true;
}

bool sd_write_sectors(uint32_t start_sector, uint32_t count, const uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++) {
        if (!sd_write_sector(start_sector + i, buf + i * 512)) return false;
    }
    return true;
}

void sd_get_info(sd_info_t *info) {
    *info = g_sd_info;
}

bool sd_is_ready(void) {
    return g_sd_info.initialized;
}
