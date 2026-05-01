#include "detect.h"
#include "../common/config.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// ============================================================
// W5500 Detection — Read version register via SPI
// ============================================================

// W5500 common register: Version (0x0039), should read 0x04
#define W5500_VERSIONR_ADDR  0x0039
#define W5500_EXPECTED_VER   0x04

// W5500 SPI frame: 16-bit addr + 8-bit control + data
// Control byte: BSB=0 (common reg), RWB=0 (read), OM=00 (variable length)
static bool probe_w5500(uint8_t *version_out) {
    // Init SPI0 for W5500
    spi_init(W5500_SPI_INST, 1000000);  // Slow for detection (1 MHz)
    gpio_set_function(W5500_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(W5500_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(W5500_PIN_SCK, GPIO_FUNC_SPI);

    // CS as GPIO output
    gpio_init(W5500_PIN_CS);
    gpio_set_dir(W5500_PIN_CS, GPIO_OUT);
    gpio_put(W5500_PIN_CS, 1);

    // RST pin — pulse low to reset
    gpio_init(W5500_PIN_RST);
    gpio_set_dir(W5500_PIN_RST, GPIO_OUT);
    gpio_put(W5500_PIN_RST, 0);
    sleep_ms(1);
    gpio_put(W5500_PIN_RST, 1);
    sleep_ms(10);  // Wait for W5500 to start

    // Read version register
    // SPI frame: [addr_hi][addr_lo][control][read_data]
    uint8_t tx[4] = {
        (W5500_VERSIONR_ADDR >> 8) & 0xFF,  // Address high
        W5500_VERSIONR_ADDR & 0xFF,          // Address low
        0x00,                                 // Control: BSB=00000, R, VDM
        0x00                                  // Dummy for read
    };
    uint8_t rx[4] = {0};

    gpio_put(W5500_PIN_CS, 0);
    spi_write_read_blocking(W5500_SPI_INST, tx, rx, 4);
    gpio_put(W5500_PIN_CS, 1);

    uint8_t version = rx[3];
    if (version_out) *version_out = version;

    // Valid if we read the expected version
    if (version == W5500_EXPECTED_VER) {
        return true;
    }

    // Cleanup — deinit SPI if not detected
    spi_deinit(W5500_SPI_INST);
    gpio_set_function(W5500_PIN_MISO, GPIO_FUNC_NULL);
    gpio_set_function(W5500_PIN_MOSI, GPIO_FUNC_NULL);
    gpio_set_function(W5500_PIN_SCK, GPIO_FUNC_NULL);
    return false;
}

// ============================================================
// SD Card Detection — Send CMD0 + CMD8
// ============================================================

// SD SPI commands
#define SD_CMD0   0x40  // GO_IDLE_STATE
#define SD_CMD8   0x48  // SEND_IF_COND
#define SD_CMD55  0x77  // APP_CMD
#define SD_ACMD41 0x69  // SD_SEND_OP_COND

static void sd_spi_send_byte(uint8_t b) {
    spi_write_blocking(SD_SPI_INST, &b, 1);
}

static uint8_t sd_spi_recv_byte(void) {
    uint8_t rx;
    uint8_t tx = 0xFF;
    spi_write_read_blocking(SD_SPI_INST, &tx, &rx, 1);
    return rx;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];
    frame[0] = cmd | 0x40;
    frame[1] = (arg >> 24) & 0xFF;
    frame[2] = (arg >> 16) & 0xFF;
    frame[3] = (arg >> 8) & 0xFF;
    frame[4] = arg & 0xFF;
    // CRC (only needed for CMD0 and CMD8)
    if (cmd == 0) frame[5] = 0x95;
    else if (cmd == 8) frame[5] = 0x87;
    else frame[5] = 0x01;

    gpio_put(SD_PIN_CS, 0);
    sd_spi_send_byte(0xFF);  // Dummy
    spi_write_blocking(SD_SPI_INST, frame, 6);

    // Wait for response (R1 format)
    uint8_t response = 0xFF;
    for (int i = 0; i < 10; i++) {
        response = sd_spi_recv_byte();
        if ((response & 0x80) == 0) break;
    }
    return response;
}

static bool probe_sd(uint8_t *type_out) {
    // Init SPI1 for SD (slow: 400 kHz for init)
    spi_init(SD_SPI_INST, 400000);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK, GPIO_FUNC_SPI);

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);

    // Send 80+ clock pulses with CS high (SD init sequence)
    uint8_t dummy[10];
    memset(dummy, 0xFF, sizeof(dummy));
    spi_write_blocking(SD_SPI_INST, dummy, 10);

    // CMD0: Reset to idle state
    uint8_t r1 = sd_send_cmd(0, 0);
    gpio_put(SD_PIN_CS, 1);
    sd_spi_send_byte(0xFF);

    if (r1 != 0x01) {
        // No card or not responding
        spi_deinit(SD_SPI_INST);
        gpio_set_function(SD_PIN_MISO, GPIO_FUNC_NULL);
        gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_NULL);
        gpio_set_function(SD_PIN_SCK, GPIO_FUNC_NULL);
        if (type_out) *type_out = 0;
        return false;
    }

    // CMD8: Check voltage range (SDv2 test)
    r1 = sd_send_cmd(8, 0x000001AA);
    uint8_t sd_type = 1;  // Default SDv1

    if (r1 == 0x01) {
        // SDv2 — read 4 bytes of R7 response
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_spi_recv_byte();
        gpio_put(SD_PIN_CS, 1);
        sd_spi_send_byte(0xFF);

        if ((r7[2] & 0x0F) == 0x01 && r7[3] == 0xAA) {
            sd_type = 3;  // SDHC/SDXC
        } else {
            sd_type = 2;  // SDv2 standard capacity
        }
    } else {
        gpio_put(SD_PIN_CS, 1);
        sd_spi_send_byte(0xFF);
    }

    if (type_out) *type_out = sd_type;

    // Leave SPI initialised (will be used by SD driver)
    // Bump speed now that card is detected
    spi_set_baudrate(SD_SPI_INST, SD_SPI_BAUD);

    return true;
}

// ============================================================
// WiFi Detection (Pico2W only — CYW43 presence)
// ============================================================

static bool probe_wifi(void) {
    // The CYW43 is connected via PIO/SPI on specific pins
    // On non-W boards, these pins are unconnected
    // Simple test: try to read CYW43 chip ID
    // For now, use compile-time detection
#ifdef CYW43_WL_GPIO_COUNT
    return true;
#else
    return false;
#endif
}

// ============================================================
// Public API
// ============================================================

void detect_probe_all(detect_result_t *result) {
    result->w5500_present = probe_w5500(&result->w5500_version);
    result->sd_present = probe_sd(&result->sd_type);
    result->wifi_present = probe_wifi();
}

node_role_t detect_role(void) {
    detect_result_t det;
    detect_probe_all(&det);

    if (det.w5500_present) return ROLE_HEAD;
    if (det.sd_present)    return ROLE_STORAGE;
    return ROLE_WORKER;
}
