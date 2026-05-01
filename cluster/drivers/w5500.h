#ifndef PICOCLUSTER_W5500_H
#define PICOCLUSTER_W5500_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// W5500 Ethernet driver (SPI mode)
// ============================================================

// Socket count
#define W5500_MAX_SOCKETS  8

// Socket states
#define W5500_SOCK_CLOSED     0x00
#define W5500_SOCK_INIT       0x13
#define W5500_SOCK_LISTEN     0x14
#define W5500_SOCK_ESTABLISHED 0x17
#define W5500_SOCK_UDP        0x22

// Protocol modes
#define W5500_PROTO_TCP  0x01
#define W5500_PROTO_UDP  0x02

typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t subnet[4];
} w5500_net_config_t;

// Initialise W5500 hardware (SPI already configured by detect)
bool w5500_init(const w5500_net_config_t *config);

// Reset chip
void w5500_reset(void);

// Check link status
bool w5500_link_up(void);

// Read chip version (should be 0x04)
uint8_t w5500_version(void);

// --- Socket operations ---

// Open a socket (returns socket number 0-7, or -1 on error)
int w5500_socket_open(uint8_t protocol, uint16_t port);

// Close a socket
void w5500_socket_close(uint8_t sock);

// Listen on a TCP socket
bool w5500_socket_listen(uint8_t sock);

// Check if connection established
bool w5500_socket_connected(uint8_t sock);

// Get socket state
uint8_t w5500_socket_status(uint8_t sock);

// Send data on a socket
int w5500_socket_send(uint8_t sock, const uint8_t *data, uint16_t len);

// Receive data from a socket (returns bytes read, 0 if none available)
int w5500_socket_recv(uint8_t sock, uint8_t *buf, uint16_t max_len);

// Send UDP datagram
int w5500_socket_sendto(uint8_t sock, const uint8_t *data, uint16_t len,
                        const uint8_t dest_ip[4], uint16_t dest_port);

// Receive UDP datagram
int w5500_socket_recvfrom(uint8_t sock, uint8_t *buf, uint16_t max_len,
                          uint8_t src_ip[4], uint16_t *src_port);

// --- Raw register access ---
void w5500_write_reg(uint16_t addr, uint8_t bsb, uint8_t data);
uint8_t w5500_read_reg(uint16_t addr, uint8_t bsb);
void w5500_write_buf(uint16_t addr, uint8_t bsb, const uint8_t *data, uint16_t len);
void w5500_read_buf(uint16_t addr, uint8_t bsb, uint8_t *data, uint16_t len);

#endif // PICOCLUSTER_W5500_H
