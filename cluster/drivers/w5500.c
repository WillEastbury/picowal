#include "w5500.h"
#include "../common/config.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// ============================================================
// W5500 SPI Register Map
// ============================================================

// Block Select Bits (BSB)
#define BSB_COMMON    0x00   // Common registers
#define BSB_SOCK(n)   (((n) << 2) + 0x01)  // Socket n registers
#define BSB_SOCK_TX(n) (((n) << 2) + 0x02) // Socket n TX buffer
#define BSB_SOCK_RX(n) (((n) << 2) + 0x03) // Socket n RX buffer

// Common registers
#define REG_MR        0x0000  // Mode
#define REG_GAR       0x0001  // Gateway (4 bytes)
#define REG_SUBR      0x0005  // Subnet (4 bytes)
#define REG_SHAR      0x0009  // MAC (6 bytes)
#define REG_SIPR      0x000F  // IP (4 bytes)
#define REG_PHYCFGR   0x002E  // PHY config
#define REG_VERSIONR  0x0039  // Version

// Socket registers (offset from socket base)
#define SOCK_MR       0x0000  // Mode
#define SOCK_CR       0x0001  // Command
#define SOCK_IR       0x0002  // Interrupt
#define SOCK_SR       0x0003  // Status
#define SOCK_PORT     0x0004  // Source port (2 bytes)
#define SOCK_DIPR     0x000C  // Dest IP (4 bytes)
#define SOCK_DPORT    0x0010  // Dest port (2 bytes)
#define SOCK_RXBUF_SIZE 0x001E
#define SOCK_TXBUF_SIZE 0x001F
#define SOCK_TX_FSR   0x0020  // TX free size (2 bytes)
#define SOCK_TX_WR    0x0024  // TX write pointer (2 bytes)
#define SOCK_RX_RSR   0x0026  // RX received size (2 bytes)
#define SOCK_RX_RD    0x0028  // RX read pointer (2 bytes)

// Socket commands
#define CMD_OPEN      0x01
#define CMD_LISTEN    0x02
#define CMD_CONNECT   0x04
#define CMD_SEND      0x20
#define CMD_RECV      0x40
#define CMD_CLOSE     0x10

// ============================================================
// Low-level SPI access
// ============================================================

static inline void cs_low(void) { gpio_put(W5500_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(W5500_PIN_CS, 1); }

void w5500_write_reg(uint16_t addr, uint8_t bsb, uint8_t data) {
    uint8_t frame[4] = {
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (bsb << 3) | 0x04,  // Write, VDM
        data
    };
    cs_low();
    spi_write_blocking(W5500_SPI_INST, frame, 4);
    cs_high();
}

uint8_t w5500_read_reg(uint16_t addr, uint8_t bsb) {
    uint8_t tx[4] = {
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (bsb << 3) | 0x00,  // Read, VDM
        0x00
    };
    uint8_t rx[4];
    cs_low();
    spi_write_read_blocking(W5500_SPI_INST, tx, rx, 4);
    cs_high();
    return rx[3];
}

void w5500_write_buf(uint16_t addr, uint8_t bsb, const uint8_t *data, uint16_t len) {
    uint8_t header[3] = {
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (bsb << 3) | 0x04  // Write, VDM
    };
    cs_low();
    spi_write_blocking(W5500_SPI_INST, header, 3);
    spi_write_blocking(W5500_SPI_INST, data, len);
    cs_high();
}

void w5500_read_buf(uint16_t addr, uint8_t bsb, uint8_t *data, uint16_t len) {
    uint8_t header[3] = {
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (bsb << 3) | 0x00  // Read, VDM
    };
    cs_low();
    spi_write_blocking(W5500_SPI_INST, header, 3);
    spi_read_blocking(W5500_SPI_INST, 0x00, data, len);
    cs_high();
}

// ============================================================
// Init / Reset
// ============================================================

void w5500_reset(void) {
    gpio_put(W5500_PIN_RST, 0);
    sleep_ms(1);
    gpio_put(W5500_PIN_RST, 1);
    sleep_ms(10);
}

uint8_t w5500_version(void) {
    return w5500_read_reg(REG_VERSIONR, BSB_COMMON);
}

bool w5500_init(const w5500_net_config_t *config) {
    // SPI already initialised by detect phase — bump to full speed
    spi_set_baudrate(W5500_SPI_INST, W5500_SPI_BAUD);

    // Soft reset
    w5500_write_reg(REG_MR, BSB_COMMON, 0x80);
    sleep_ms(10);

    // Verify
    if (w5500_version() != 0x04) return false;

    // Set network config
    w5500_write_buf(REG_GAR, BSB_COMMON, config->gateway, 4);
    w5500_write_buf(REG_SUBR, BSB_COMMON, config->subnet, 4);
    w5500_write_buf(REG_SHAR, BSB_COMMON, config->mac, 6);
    w5500_write_buf(REG_SIPR, BSB_COMMON, config->ip, 4);

    // Set all socket buffer sizes to 2KB each (default)
    for (uint8_t i = 0; i < W5500_MAX_SOCKETS; i++) {
        w5500_write_reg(SOCK_RXBUF_SIZE, BSB_SOCK(i), 2);  // 2KB
        w5500_write_reg(SOCK_TXBUF_SIZE, BSB_SOCK(i), 2);  // 2KB
    }

    return true;
}

bool w5500_link_up(void) {
    uint8_t phy = w5500_read_reg(REG_PHYCFGR, BSB_COMMON);
    return (phy & 0x01) != 0;  // Link bit
}

// ============================================================
// Socket operations
// ============================================================

int w5500_socket_open(uint8_t protocol, uint16_t port) {
    // Find a free socket
    for (uint8_t i = 0; i < W5500_MAX_SOCKETS; i++) {
        uint8_t status = w5500_read_reg(SOCK_SR, BSB_SOCK(i));
        if (status == W5500_SOCK_CLOSED) {
            // Configure
            w5500_write_reg(SOCK_MR, BSB_SOCK(i), protocol);
            w5500_write_reg(SOCK_PORT, BSB_SOCK(i), (port >> 8) & 0xFF);
            w5500_write_reg(SOCK_PORT + 1, BSB_SOCK(i), port & 0xFF);

            // Open
            w5500_write_reg(SOCK_CR, BSB_SOCK(i), CMD_OPEN);
            sleep_us(100);

            return (int)i;
        }
    }
    return -1;  // No free sockets
}

void w5500_socket_close(uint8_t sock) {
    w5500_write_reg(SOCK_CR, BSB_SOCK(sock), CMD_CLOSE);
    sleep_us(100);
}

bool w5500_socket_listen(uint8_t sock) {
    w5500_write_reg(SOCK_CR, BSB_SOCK(sock), CMD_LISTEN);
    sleep_us(100);
    return w5500_read_reg(SOCK_SR, BSB_SOCK(sock)) == W5500_SOCK_LISTEN;
}

bool w5500_socket_connected(uint8_t sock) {
    return w5500_read_reg(SOCK_SR, BSB_SOCK(sock)) == W5500_SOCK_ESTABLISHED;
}

uint8_t w5500_socket_status(uint8_t sock) {
    return w5500_read_reg(SOCK_SR, BSB_SOCK(sock));
}

int w5500_socket_send(uint8_t sock, const uint8_t *data, uint16_t len) {
    // Check free TX buffer space
    uint16_t free_size = ((uint16_t)w5500_read_reg(SOCK_TX_FSR, BSB_SOCK(sock)) << 8) |
                          w5500_read_reg(SOCK_TX_FSR + 1, BSB_SOCK(sock));

    if (free_size < len) return 0;  // Not enough space

    // Get TX write pointer
    uint16_t ptr = ((uint16_t)w5500_read_reg(SOCK_TX_WR, BSB_SOCK(sock)) << 8) |
                    w5500_read_reg(SOCK_TX_WR + 1, BSB_SOCK(sock));

    // Write data to TX buffer
    w5500_write_buf(ptr, BSB_SOCK_TX(sock), data, len);

    // Update write pointer
    ptr += len;
    w5500_write_reg(SOCK_TX_WR, BSB_SOCK(sock), (ptr >> 8) & 0xFF);
    w5500_write_reg(SOCK_TX_WR + 1, BSB_SOCK(sock), ptr & 0xFF);

    // Issue SEND command
    w5500_write_reg(SOCK_CR, BSB_SOCK(sock), CMD_SEND);

    return (int)len;
}

int w5500_socket_recv(uint8_t sock, uint8_t *buf, uint16_t max_len) {
    // Check received data size
    uint16_t rx_size = ((uint16_t)w5500_read_reg(SOCK_RX_RSR, BSB_SOCK(sock)) << 8) |
                        w5500_read_reg(SOCK_RX_RSR + 1, BSB_SOCK(sock));

    if (rx_size == 0) return 0;
    if (rx_size > max_len) rx_size = max_len;

    // Get RX read pointer
    uint16_t ptr = ((uint16_t)w5500_read_reg(SOCK_RX_RD, BSB_SOCK(sock)) << 8) |
                    w5500_read_reg(SOCK_RX_RD + 1, BSB_SOCK(sock));

    // Read data
    w5500_read_buf(ptr, BSB_SOCK_RX(sock), buf, rx_size);

    // Update read pointer
    ptr += rx_size;
    w5500_write_reg(SOCK_RX_RD, BSB_SOCK(sock), (ptr >> 8) & 0xFF);
    w5500_write_reg(SOCK_RX_RD + 1, BSB_SOCK(sock), ptr & 0xFF);

    // Issue RECV command
    w5500_write_reg(SOCK_CR, BSB_SOCK(sock), CMD_RECV);

    return (int)rx_size;
}

int w5500_socket_sendto(uint8_t sock, const uint8_t *data, uint16_t len,
                        const uint8_t dest_ip[4], uint16_t dest_port) {
    // Set destination
    w5500_write_buf(SOCK_DIPR, BSB_SOCK(sock), dest_ip, 4);
    w5500_write_reg(SOCK_DPORT, BSB_SOCK(sock), (dest_port >> 8) & 0xFF);
    w5500_write_reg(SOCK_DPORT + 1, BSB_SOCK(sock), dest_port & 0xFF);

    return w5500_socket_send(sock, data, len);
}

int w5500_socket_recvfrom(uint8_t sock, uint8_t *buf, uint16_t max_len,
                          uint8_t src_ip[4], uint16_t *src_port) {
    int n = w5500_socket_recv(sock, buf, max_len);
    if (n > 0 && src_ip) {
        w5500_read_buf(SOCK_DIPR, BSB_SOCK(sock), src_ip, 4);
    }
    if (n > 0 && src_port) {
        *src_port = ((uint16_t)w5500_read_reg(SOCK_DPORT, BSB_SOCK(sock)) << 8) |
                     w5500_read_reg(SOCK_DPORT + 1, BSB_SOCK(sock));
    }
    return n;
}
