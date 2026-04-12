#include "sd_card.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "httpd/web_server.h"
#include <stdio.h>
#include <string.h>

// ============================================================
// SD Card driver — PIO-based SPI on arbitrary pins
// GP5=CLK, GP18=MOSI, GP19=MISO, GP22=CS (manual)
// ============================================================

#include "hardware/spi.h"
#define SD_SPI spi1
static bool g_sd_ready = false;
static bool g_sd_sdhc = false;
static uint32_t g_sd_blocks = 0;
static char g_sd_debug[128] = "not initialized";

#define CMD0    0
#define CMD8    8
#define CMD9    9
#define CMD12   12
#define CMD16   16
#define CMD17   17
#define CMD18   18
#define CMD24   24
#define CMD25   25
#define CMD55   55
#define CMD58   58
#define ACMD41  41

static uint8_t spi_transfer(uint8_t tx);

static inline void cs_select(void) { gpio_put(SD_CS_PIN, 0); }
static inline void cs_deselect(void) {
    gpio_put(SD_CS_PIN, 1);
    spi_transfer(0xFF);
}

// Send one byte, receive one byte simultaneously
static uint8_t spi_transfer(uint8_t tx) {
    uint8_t rx;
    // Timeout guard: if SPI1 hangs (no card, bus stuck), bail after 10ms
    absolute_time_t deadline = make_timeout_time_ms(10);
    while (!spi_is_writable(SD_SPI)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return 0xFF;
    }
    spi_write_read_blocking(SD_SPI, &tx, &rx, 1);
    return rx;
}

static void spi_write(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) spi_transfer(data[i]);
}

static void spi_read(uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) data[i] = spi_transfer(0xFF);
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t frame[6] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8), (uint8_t)(arg),
        cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01
    };
    cs_select();
    spi_transfer(0xFF);
    spi_write(frame, 6);
    uint8_t r1;
    for (int i = 0; i < 10; i++) {
        r1 = spi_transfer(0xFF);
        if (!(r1 & 0x80)) break;
    }
    return r1;
}

static uint8_t sd_cmd_end(uint8_t cmd, uint32_t arg) {
    uint8_t r1 = sd_cmd(cmd, arg);
    cs_deselect();
    return r1;
}

static bool sd_wait_token(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t b = spi_transfer(0xFF);
        if (b == 0xFE) return true;
        if (b != 0xFF) return false;
    }
    return false;
}



bool sd_init(void) {
    g_sd_ready = false;
    g_sd_sdhc = false;
    g_sd_blocks = 0;

    // Overall timeout: if sd_init takes longer than 5 seconds, bail
    absolute_time_t sd_deadline = make_timeout_time_ms(5000);

    web_log("[sd] SPI1 init: CLK=%d MOSI=%d MISO=%d CS=%d\n",
            SD_CLK_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CS_PIN);

    // Init CS pin
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    gpio_put(SD_CS_PIN, 1);

    // Deselect LCD and touch before using SPI1 for SD
    gpio_put(9, 1);   // LCD_CS high
    gpio_put(16, 1);  // TP_CS high
    // Slow SPI1 to 400kHz for SD card init sequence (required by spec)
    spi_set_baudrate(SD_SPI, 400000);
    gpio_pull_up(SD_MISO_PIN);
    web_log("[sd] SPI1 slowed to 400kHz for init\n");

    sleep_ms(100);

    // 160+ clocks with CS high to enter SPI mode
    gpio_put(SD_CS_PIN, 1);
    for (int i = 0; i < 20; i++) spi_transfer(0xFF);

    // CMD0: GO_IDLE — must get 0x01
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10; i++) {
        if (absolute_time_diff_us(get_absolute_time(), sd_deadline) < 0) {
            web_log("[sd] TIMEOUT during CMD0\n");
            spi_set_baudrate(SD_SPI, 30000000);
            return false;
        }
        r1 = sd_cmd_end(CMD0, 0);
        web_log("[sd] CMD0 attempt %d: r1=0x%02x\n", i, r1);
        if (r1 == 0x01 || r1 == 0x00) break;
        sleep_ms(100);
    }
    if (r1 != 0x01 && r1 != 0x00) {
        snprintf(g_sd_debug, sizeof(g_sd_debug), "FAIL: CMD0 r1=0x%02x (need 0x01 or 0x00)", r1);
        web_log("[sd] %s\n", g_sd_debug);
        spi_set_baudrate(SD_SPI, 30000000);
        return false;
    }

    // CMD8: check v2
    web_log("[sd] CMD0 r1=0x%02x, sending CMD8\n", r1);
    bool was_idle = (r1 == 0x01);
    r1 = sd_cmd(CMD8, 0x000001AA);
    bool v2 = (r1 == 0x01 || r1 == 0x00);  // accept 0x00 if card was already ready
    if (r1 == 0x01 || r1 == 0x00) {
        uint8_t r7[4]; spi_read(r7, 4);
        web_log("[sd] CMD8 r7=%02x%02x%02x%02x\n", r7[0], r7[1], r7[2], r7[3]);
        if (r7[2] != 0x01 || r7[3] != 0xAA) {
            snprintf(g_sd_debug, sizeof(g_sd_debug), "FAIL: CMD8 pattern");
            cs_deselect(); return false;
        }
    }
    cs_deselect();

    // ACMD41: init
    web_log("[sd] CMD8 v2=%d, starting ACMD41\n", v2);
    uint32_t a41 = v2 ? 0x40000000 : 0;
    for (int i = 0; i < 1000; i++) {
        if (absolute_time_diff_us(get_absolute_time(), sd_deadline) < 0) {
            web_log("[sd] TIMEOUT during ACMD41\n");
            spi_set_baudrate(SD_SPI, 30000000);
            return false;
        }
        sd_cmd_end(CMD55, 0);
        r1 = sd_cmd_end(ACMD41, a41);
        if (r1 == 0x00) break;
        sleep_ms(1);
    }
    if (r1 != 0x00) {
        snprintf(g_sd_debug, sizeof(g_sd_debug), "FAIL: ACMD41 r1=0x%02x", r1);
        web_log("[sd] %s\n", g_sd_debug);
        spi_set_baudrate(SD_SPI, 30000000);
        return false;
    }

    // CMD58: OCR — check SDHC
    g_sd_sdhc = false;
    if (v2) {
        r1 = sd_cmd(CMD58, 0);
        if (r1 == 0x00) {
            uint8_t ocr[4]; spi_read(ocr, 4);
            g_sd_sdhc = (ocr[0] & 0x40) != 0;
            web_log("[sd] OCR=%02x%02x%02x%02x SDHC=%d\n", ocr[0], ocr[1], ocr[2], ocr[3], g_sd_sdhc);
        }
        cs_deselect();
    }

    if (!g_sd_sdhc) sd_cmd_end(CMD16, 512);

    // Restore SPI1 to high speed — works for both LCD (up to 60MHz) and SD (up to 25MHz)
    spi_set_baudrate(SD_SPI, 25000000);
    web_log("[sd] SPI1 restored to 25MHz\n");

    // Read CSD for capacity
    web_log("[sd] Reading CSD...\n");
    r1 = sd_cmd(CMD9, 0);
    web_log("[sd] CMD9 r1=0x%02x, waiting for data token...\n", r1);
    bool got_token = false;
    if (r1 == 0x00) {
        // Wait for data token with timeout
        for (int t = 0; t < 200000; t++) {
            if (absolute_time_diff_us(get_absolute_time(), sd_deadline) < 0) {
                web_log("[sd] TIMEOUT waiting for CSD token\n");
                cs_deselect();
                spi_set_baudrate(SD_SPI, 30000000);
                return false;
            }
            uint8_t b = spi_transfer(0xFF);
            if (b == 0xFE) { got_token = true; break; }
            if (b != 0xFF) { web_log("[sd] CSD got 0x%02x instead of token\n", b); break; }
        }
    }
    if (r1 == 0x00 && got_token) {
        uint8_t csd[16], crc[2];
        spi_read(csd, 16); spi_read(crc, 2);

        snprintf(g_sd_debug, sizeof(g_sd_debug),
            "CSD: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x v=%u",
            csd[0],csd[1],csd[2],csd[3],csd[4],csd[5],csd[6],csd[7],
            csd[8],csd[9],csd[10],csd[11],csd[12],csd[13],csd[14],csd[15],
            (unsigned)(csd[0] >> 6));
        web_log("[sd] %s\n", g_sd_debug);

        if ((csd[0] >> 6) == 1) {
            uint32_t c = ((uint32_t)(csd[7] & 0x3F) << 16) |
                         ((uint32_t)csd[8] << 8) | csd[9];
            g_sd_blocks = (c + 1) * 1024;
        } else {
            uint32_t c = ((uint32_t)(csd[6] & 0x03) << 10) |
                         ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
            uint32_t m = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
            uint32_t bl = csd[5] & 0x0F;
            g_sd_blocks = (c + 1) * (1u << (m + 2)) * (1u << bl) / 512;
        }
    } else {
        snprintf(g_sd_debug, sizeof(g_sd_debug), "CSD read failed r1=0x%02x", r1);
        web_log("[sd] %s\n", g_sd_debug);
    }
    cs_deselect();

    g_sd_ready = true;
    web_log("[sd] Ready: %s, %lu blocks (%lu MB)\n",
           g_sd_sdhc ? "SDHC" : "SDSC",
           (unsigned long)g_sd_blocks,
           (unsigned long)(g_sd_blocks / 2048));
    return true;
}

bool sd_read_block(uint32_t block_addr, uint8_t *buf) {
    if (!g_sd_ready) return false;
    uint32_t addr = g_sd_sdhc ? block_addr : (block_addr * 512);
    if (sd_cmd(CMD17, addr) != 0x00) { cs_deselect(); return false; }
    if (!sd_wait_token()) { cs_deselect(); return false; }
    spi_read(buf, 512);
    spi_transfer(0xFF); spi_transfer(0xFF);
    cs_deselect();
    return true;
}

bool sd_write_block(uint32_t block_addr, const uint8_t *buf) {
    if (!g_sd_ready) return false;
    uint32_t addr = g_sd_sdhc ? block_addr : (block_addr * 512);
    if (sd_cmd(CMD24, addr) != 0x00) { cs_deselect(); return false; }
    spi_transfer(0xFE);
    spi_write(buf, 512);
    spi_transfer(0xFF); spi_transfer(0xFF);
    uint8_t resp = spi_transfer(0xFF);
    if ((resp & 0x1F) != 0x05) { cs_deselect(); return false; }
    for (int i = 0; i < 500000; i++) { if (spi_transfer(0xFF) != 0x00) break; }
    cs_deselect();
    return true;
}

bool sd_read_blocks(uint32_t block_addr, uint8_t *buf, uint32_t count) {
    if (!g_sd_ready || count == 0) return false;
    if (count == 1) return sd_read_block(block_addr, buf);
    uint32_t addr = g_sd_sdhc ? block_addr : (block_addr * 512);
    if (sd_cmd(CMD18, addr) != 0x00) { cs_deselect(); return false; }
    for (uint32_t i = 0; i < count; i++) {
        if (!sd_wait_token()) { sd_cmd_end(CMD12, 0); return false; }
        spi_read(buf + i * 512, 512);
        spi_transfer(0xFF); spi_transfer(0xFF);
    }
    sd_cmd(CMD12, 0); spi_transfer(0xFF);
    cs_deselect();
    return true;
}

bool sd_write_blocks(uint32_t block_addr, const uint8_t *buf, uint32_t count) {
    if (!g_sd_ready || count == 0) return false;
    if (count == 1) return sd_write_block(block_addr, buf);
    uint32_t addr = g_sd_sdhc ? block_addr : (block_addr * 512);
    if (sd_cmd(CMD25, addr) != 0x00) { cs_deselect(); return false; }
    for (uint32_t i = 0; i < count; i++) {
        spi_transfer(0xFC);
        spi_write(buf + i * 512, 512);
        spi_transfer(0xFF); spi_transfer(0xFF);
        if ((spi_transfer(0xFF) & 0x1F) != 0x05) { cs_deselect(); return false; }
        for (int j = 0; j < 500000; j++) { if (spi_transfer(0xFF) != 0x00) break; }
    }
    spi_transfer(0xFD);
    for (int i = 0; i < 500000; i++) { if (spi_transfer(0xFF) != 0x00) break; }
    cs_deselect();
    return true;
}

bool sd_get_info(sd_info_t *info) {
    if (!g_sd_ready) return false;
    info->block_count = g_sd_blocks;
    info->capacity_mb = g_sd_blocks / 2048;
    info->sdhc = g_sd_sdhc;
    return true;
}

const char *sd_get_debug(void) { return g_sd_debug; }
