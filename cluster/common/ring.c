#include "ring.h"
#include "../common/config.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "ring_uart.pio.h"  // Generated from .pio file

// ============================================================
// PIO Link Driver — Point-to-point Manchester serial links
// ============================================================
// Star topology: workers have 1 link (TX+RX to head).
// Heads have 6 links (one per connected node) + 1 interlink.
// Same PIO programs (Manchester TX/RX), different topology.

// --- Worker link state (single link to head) ---
static struct {
    PIO      pio;
    uint     sm_tx;
    uint     sm_rx;
    uint     dma_tx;
    uint     dma_rx;
    uint8_t  rx_buf[2][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint8_t  tx_buf[PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint8_t  rx_active;  // Double-buffer index
    bool     rx_ready;
} worker_link;

// --- Head port state (6 ports + interlink) ---
#define HEAD_MAX_PORTS (PORTS_PER_HEAD + 1)  // +1 for interlink

typedef struct {
    PIO      pio;
    uint     sm_tx;
    uint     sm_rx;
    uint     dma_tx;
    uint     dma_rx;
    uint8_t  pin_tx;
    uint8_t  pin_rx;
    uint8_t  rx_buf[2][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint8_t  tx_buf[PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
    uint8_t  rx_active;
    bool     rx_ready;
    uint8_t  node_id;    // ID of node on this port (0xFF = empty)
} head_port_t;

static head_port_t head_ports[HEAD_MAX_PORTS];
static bool is_head_mode = false;

// ============================================================
// Common PIO setup
// ============================================================

static uint g_tx_offset = 0xFFFF;
static uint g_rx_offset = 0xFFFF;

static void load_pio_programs(PIO pio) {
    if (g_tx_offset == 0xFFFF) {
        g_tx_offset = pio_add_program(pio, &ring_tx_program);
    }
    if (g_rx_offset == 0xFFFF) {
        g_rx_offset = pio_add_program(pio, &ring_rx_program);
    }
}

static void configure_tx_sm(PIO pio, uint sm, uint pin) {
    pio_sm_config cfg = ring_tx_program_get_default_config(g_tx_offset);
    sm_config_set_sideset_pins(&cfg, pin);
    sm_config_set_out_shift(&cfg, false, true, 32);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);
    sm_config_set_clkdiv(&cfg, div);

    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_init(pio, sm, g_tx_offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}

static void configure_rx_sm(PIO pio, uint sm, uint pin) {
    pio_sm_config cfg = ring_rx_program_get_default_config(g_rx_offset);
    sm_config_set_in_pins(&cfg, pin);
    sm_config_set_in_shift(&cfg, false, true, 32);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);
    sm_config_set_clkdiv(&cfg, div);

    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);
    pio_sm_init(pio, sm, g_rx_offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}

static void setup_rx_dma(uint channel, PIO pio, uint sm, uint8_t *buf, uint32_t len) {
    dma_channel_config c = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(channel, &c, buf, &pio->rxf[sm], len, true);
}

// ============================================================
// Worker API (single link to head)
// ============================================================

void link_worker_init(void) {
    is_head_mode = false;

    worker_link.pio = pio0;
    worker_link.sm_tx = 0;
    worker_link.sm_rx = 1;
    worker_link.dma_tx = DMA_CH_LINK_TX;
    worker_link.dma_rx = DMA_CH_LINK_RX;
    worker_link.rx_active = 0;
    worker_link.rx_ready = false;

    load_pio_programs(pio0);
    configure_tx_sm(pio0, 0, LINK_PIN_TX);
    configure_rx_sm(pio0, 1, LINK_PIN_RX);

    // Start RX DMA
    setup_rx_dma(DMA_CH_LINK_RX, pio0, 1,
                 worker_link.rx_buf[0], PKT_MAX_PAYLOAD + PKT_HEADER_SIZE);
}

bool link_worker_poll_rx(pkt_header_t *hdr, uint8_t **payload) {
    if (!dma_channel_is_busy(worker_link.dma_rx)) {
        // RX complete — swap buffers
        uint8_t *buf = worker_link.rx_buf[worker_link.rx_active];
        worker_link.rx_active ^= 1;

        // Restart DMA on other buffer
        setup_rx_dma(worker_link.dma_rx, worker_link.pio, worker_link.sm_rx,
                     worker_link.rx_buf[worker_link.rx_active],
                     PKT_MAX_PAYLOAD + PKT_HEADER_SIZE);

        // Parse header
        memcpy(hdr, buf, PKT_HEADER_SIZE);
        *payload = buf + PKT_HEADER_SIZE;

        // Verify CRC
        uint16_t crc = crc16_ccitt(buf + PKT_HEADER_SIZE, hdr->payload_len);
        if (crc != hdr->crc16) return false;

        return true;
    }
    return false;
}

void link_worker_send(const pkt_header_t *hdr, const uint8_t *payload, uint16_t len) {
    // Wait for previous TX to complete
    dma_channel_wait_for_finish_blocking(worker_link.dma_tx);

    // Assemble packet
    memcpy(worker_link.tx_buf, hdr, PKT_HEADER_SIZE);
    if (payload && len > 0) {
        memcpy(worker_link.tx_buf + PKT_HEADER_SIZE, payload, len);
    }

    // Fire DMA
    uint32_t total = PKT_HEADER_SIZE + len;
    dma_channel_config c = dma_channel_get_default_config(worker_link.dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(worker_link.pio, worker_link.sm_tx, true));

    dma_channel_configure(worker_link.dma_tx, &c,
                          &worker_link.pio->txf[worker_link.sm_tx],
                          worker_link.tx_buf, total, true);
}

// ============================================================
// Head API (PIO switch fabric — 6 ports + interlink)
// ============================================================

// Pin pairs for each port
static const uint8_t port_pins[HEAD_MAX_PORTS][2] = {
    { HEAD_PORT0_TX, HEAD_PORT0_RX },
    { HEAD_PORT1_TX, HEAD_PORT1_RX },
    { HEAD_PORT2_TX, HEAD_PORT2_RX },
    { HEAD_PORT3_TX, HEAD_PORT3_RX },
    { HEAD_PORT4_TX, HEAD_PORT4_RX },
    { HEAD_PORT5_TX, HEAD_PORT5_RX },
    { HEAD_INTERLINK_TX, HEAD_INTERLINK_RX },  // Port 6 = interlink
};

// Map port index to PIO block and SM offset
static PIO port_pio(uint8_t port) {
    if (port < 2) return pio0;
    if (port < 4) return pio1;
    return pio2;
}

static uint port_sm_tx(uint8_t port) {
    return (port % 2) * 2;       // 0, 2, 0, 2, 0, 2, 0
}

static uint port_sm_rx(uint8_t port) {
    return (port % 2) * 2 + 1;   // 1, 3, 1, 3, 1, 3, 1
}

void link_head_init(void) {
    is_head_mode = true;

    // Load PIO programs into all 3 blocks
    load_pio_programs(pio0);
    g_tx_offset = pio_add_program(pio1, &ring_tx_program);
    g_rx_offset = pio_add_program(pio1, &ring_rx_program);
    pio_add_program(pio2, &ring_tx_program);
    pio_add_program(pio2, &ring_rx_program);

    for (uint8_t p = 0; p < HEAD_MAX_PORTS; p++) {
        head_port_t *port = &head_ports[p];
        port->pio = port_pio(p);
        port->sm_tx = port_sm_tx(p);
        port->sm_rx = port_sm_rx(p);
        port->dma_tx = DMA_CH_PORT0_TX + p * 2;
        port->dma_rx = DMA_CH_PORT0_RX + p * 2;
        port->pin_tx = port_pins[p][0];
        port->pin_rx = port_pins[p][1];
        port->rx_active = 0;
        port->rx_ready = false;
        port->node_id = 0xFF;

        configure_tx_sm(port->pio, port->sm_tx, port->pin_tx);
        configure_rx_sm(port->pio, port->sm_rx, port->pin_rx);

        // Start RX DMA
        setup_rx_dma(port->dma_rx, port->pio, port->sm_rx,
                     port->rx_buf[0], PKT_MAX_PAYLOAD + PKT_HEADER_SIZE);
    }
}

// Poll a specific port for incoming packet
bool link_head_poll_port(uint8_t port, pkt_header_t *hdr, uint8_t **payload) {
    if (port >= HEAD_MAX_PORTS) return false;
    head_port_t *p = &head_ports[port];

    if (!dma_channel_is_busy(p->dma_rx)) {
        uint8_t *buf = p->rx_buf[p->rx_active];
        p->rx_active ^= 1;

        setup_rx_dma(p->dma_rx, p->pio, p->sm_rx,
                     p->rx_buf[p->rx_active],
                     PKT_MAX_PAYLOAD + PKT_HEADER_SIZE);

        memcpy(hdr, buf, PKT_HEADER_SIZE);
        *payload = buf + PKT_HEADER_SIZE;

        uint16_t crc = crc16_ccitt(buf + PKT_HEADER_SIZE, hdr->payload_len);
        if (crc != hdr->crc16) return false;

        return true;
    }
    return false;
}

// Send packet out a specific port
void link_head_send_port(uint8_t port, const pkt_header_t *hdr,
                         const uint8_t *payload, uint16_t len) {
    if (port >= HEAD_MAX_PORTS) return;
    head_port_t *p = &head_ports[port];

    dma_channel_wait_for_finish_blocking(p->dma_tx);

    memcpy(p->tx_buf, hdr, PKT_HEADER_SIZE);
    if (payload && len > 0) {
        memcpy(p->tx_buf + PKT_HEADER_SIZE, payload, len);
    }

    uint32_t total = PKT_HEADER_SIZE + len;
    dma_channel_config c = dma_channel_get_default_config(p->dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(p->pio, p->sm_tx, true));

    dma_channel_configure(p->dma_tx, &c,
                          &p->pio->txf[p->sm_tx],
                          p->tx_buf, total, true);
}

// Broadcast to all connected ports (except source)
void link_head_broadcast(const pkt_header_t *hdr, const uint8_t *payload,
                         uint16_t len, uint8_t except_port) {
    for (uint8_t p = 0; p < PORTS_PER_HEAD; p++) {
        if (p == except_port) continue;
        if (head_ports[p].node_id == 0xFF) continue;  // No node on port
        link_head_send_port(p, hdr, payload, len);
    }
}

// Route a packet: look up dest node_id → port, send it there
bool link_head_route(const pkt_header_t *hdr, const uint8_t *payload, uint16_t len) {
    // Broadcast?
    if (hdr->dest == ADDR_BROADCAST) {
        link_head_broadcast(hdr, payload, len, 0xFF);
        return true;
    }

    // Find port for destination
    for (uint8_t p = 0; p < PORTS_PER_HEAD; p++) {
        if (head_ports[p].node_id == hdr->dest) {
            link_head_send_port(p, hdr, payload, len);
            return true;
        }
    }

    // Not on this head — send to interlink (port 6)
    link_head_send_port(PORTS_PER_HEAD, hdr, payload, len);
    return true;
}

// Register a node ID on a port (called during discovery)
void link_head_set_port_node(uint8_t port, uint8_t node_id) {
    if (port < HEAD_MAX_PORTS) {
        head_ports[port].node_id = node_id;
    }
}

// Get port for a node_id (returns 0xFF if not found)
uint8_t link_head_find_port(uint8_t node_id) {
    for (uint8_t p = 0; p < HEAD_MAX_PORTS; p++) {
        if (head_ports[p].node_id == node_id) return p;
    }
    return 0xFF;
}

    pio_sm_init(pio, sm_tx, tx_offset, &tx_cfg);
    pio_sm_set_enabled(pio, sm_tx, true);

    // Load RX program
    uint rx_offset = pio_add_program(pio, &ring_rx_program);

    // Configure RX SM
    pio_sm_config rx_cfg = ring_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_cfg, config->pin_rx);
    sm_config_set_in_shift(&rx_cfg, false, true, 32);  // Shift left, autopush, 32 bits
    sm_config_set_clkdiv(&rx_cfg, div);

    pio_gpio_init(pio, config->pin_rx);
    pio_sm_set_consecutive_pindirs(pio, sm_rx, config->pin_rx, 1, false);

    pio_sm_init(pio, sm_rx, rx_offset, &rx_cfg);
    pio_sm_set_enabled(pio, sm_rx, true);

    // Configure DMA for RX (double buffered)
    dma_channel_config rx_dma_cfg = dma_channel_get_default_config(config->dma_ch_rx);
    channel_config_set_transfer_data_size(&rx_dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&rx_dma_cfg, false);
    channel_config_set_write_increment(&rx_dma_cfg, true);
    channel_config_set_dreq(&rx_dma_cfg, pio_get_dreq(pio, sm_rx, false));

    dma_channel_configure(
        config->dma_ch_rx,
        &rx_dma_cfg,
        ring->rx_buf[0],                     // Write to buffer 0
        &pio->rxf[sm_rx],                    // Read from PIO RX FIFO
        (PKT_MAX_PAYLOAD + PKT_HEADER_SIZE) / 4,  // Transfer count (words)
        true                                  // Start immediately
    );

    ring->rx_active_buf = 0;

    // Configure DMA for TX
    dma_channel_config tx_dma_cfg = dma_channel_get_default_config(config->dma_ch_tx);
    channel_config_set_transfer_data_size(&tx_dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&tx_dma_cfg, true);
    channel_config_set_write_increment(&tx_dma_cfg, false);
    channel_config_set_dreq(&tx_dma_cfg, pio_get_dreq(pio, sm_tx, true));

    // TX DMA not started until we have data to send
}

void ring_init_all(const ring_config_t configs[RING_COUNT]) {
    for (uint8_t i = 0; i < RING_COUNT; i++) {
        ring_init(i, &configs[i]);
    }
}

// ============================================================
// Receive
// ============================================================

bool ring_poll_rx(uint8_t ring_id, pkt_header_t *hdr, uint8_t **payload) {
    ring_state_t *ring = &rings[ring_id];

    // Check if DMA has completed (packet received)
    if (dma_channel_is_busy(ring->config.dma_ch_rx)) {
        return false;  // Still receiving
    }

    // How many words were transferred?
    uint32_t remaining = dma_channel_hw_addr(ring->config.dma_ch_rx)->transfer_count;
    uint32_t total_words = ((PKT_MAX_PAYLOAD + PKT_HEADER_SIZE) / 4) - remaining;
    if (total_words < PKT_HEADER_SIZE / 4) {
        // Too short — re-arm and skip
        ring_rx_done(ring_id);
        return false;
    }

    uint8_t *buf = ring->rx_buf[ring->rx_active_buf];

    // Parse header
    memcpy(hdr, buf, PKT_HEADER_SIZE);

    // Validate payload length
    if (hdr->payload_len > PKT_MAX_PAYLOAD) {
        ring_rx_done(ring_id);
        return false;
    }

    *payload = buf + PKT_HEADER_SIZE;
    ring->packets_rx++;

    // Call snoop callback before deciding what to do with it
    if (g_snoop_cb) {
        g_snoop_cb(ring_id, hdr, *payload);
    }

    return true;
}

void ring_rx_done(uint8_t ring_id) {
    ring_state_t *ring = &rings[ring_id];

    // Flip to other buffer and re-arm DMA
    ring->rx_active_buf ^= 1;

    PIO pio = pio_for_ring(ring_id);
    dma_channel_config cfg = dma_channel_get_default_config(ring->config.dma_ch_rx);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, ring->config.sm_rx, false));

    dma_channel_configure(
        ring->config.dma_ch_rx,
        &cfg,
        ring->rx_buf[ring->rx_active_buf],
        &pio->rxf[ring->config.sm_rx],
        (PKT_MAX_PAYLOAD + PKT_HEADER_SIZE) / 4,
        true
    );
}

// ============================================================
// Transmit
// ============================================================

bool ring_send(uint8_t ring_id, const pkt_header_t *hdr,
               const uint8_t *payload, uint32_t payload_len) {
    ring_state_t *ring = &rings[ring_id];

    if (ring->tx_busy) return false;

    // Assemble packet into TX buffer
    memcpy(ring->tx_buf, hdr, PKT_HEADER_SIZE);
    if (payload && payload_len > 0) {
        memcpy(ring->tx_buf + PKT_HEADER_SIZE, payload, payload_len);
    }

    uint32_t total_len = PKT_HEADER_SIZE + payload_len;
    uint32_t total_words = (total_len + 3) / 4;

    ring->tx_len = total_len;
    ring->tx_busy = true;

    // Start DMA transfer to PIO TX FIFO
    PIO pio = pio_for_ring(ring_id);
    dma_channel_config cfg = dma_channel_get_default_config(ring->config.dma_ch_tx);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, ring->config.sm_tx, true));

    dma_channel_configure(
        ring->config.dma_ch_tx,
        &cfg,
        &pio->txf[ring->config.sm_tx],
        ring->tx_buf,
        total_words,
        true
    );

    ring->packets_tx++;
    return true;
}

void ring_forward(uint8_t src_ring, uint8_t dst_ring) {
    ring_state_t *src = &rings[src_ring];
    ring_state_t *dst = &rings[dst_ring];

    if (dst->tx_busy) return;  // Drop if TX busy (should rarely happen)

    // Copy from src RX buffer to dst TX buffer
    uint8_t *rx_buf = src->rx_buf[src->rx_active_buf];
    pkt_header_t hdr;
    memcpy(&hdr, rx_buf, PKT_HEADER_SIZE);

    uint32_t total_len = PKT_HEADER_SIZE + hdr.payload_len;
    memcpy(dst->tx_buf, rx_buf, total_len);

    uint32_t total_words = (total_len + 3) / 4;
    dst->tx_len = total_len;
    dst->tx_busy = true;

    PIO pio = pio_for_ring(dst_ring);
    dma_channel_config cfg = dma_channel_get_default_config(dst->config.dma_ch_tx);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, dst->config.sm_tx, true));

    dma_channel_configure(
        dst->config.dma_ch_tx,
        &cfg,
        &pio->txf[dst->config.sm_tx],
        dst->tx_buf,
        total_words,
        true
    );

    dst->packets_forwarded++;
}

bool ring_tx_ready(uint8_t ring_id) {
    ring_state_t *ring = &rings[ring_id];
    if (ring->tx_busy) {
        // Check if DMA finished
        if (!dma_channel_is_busy(ring->config.dma_ch_tx)) {
            ring->tx_busy = false;
        }
    }
    return !ring->tx_busy;
}

void ring_set_snoop_callback(ring_snoop_cb cb) {
    g_snoop_cb = cb;
}

// ============================================================
// Packet processing helper
// ============================================================

bool ring_process_packet(uint8_t ring_id, uint8_t my_node_id) {
    pkt_header_t hdr;
    uint8_t *payload;

    if (!ring_poll_rx(ring_id, &hdr, &payload)) return false;

    // Verify CRC
    uint16_t expected = hdr.crc16;
    if (hdr.payload_len > 0) {
        uint16_t calc = crc16_ccitt(payload, hdr.payload_len);
        if (calc != expected) {
            rings[ring_id].crc_errors++;
            ring_rx_done(ring_id);
            return false;
        }
    }

    // Is it for us?
    if (hdr.dest == my_node_id || hdr.dest == ADDR_BROADCAST) {
        // Consumed locally — caller handles via separate mechanism
        return true;
    }

    // Not for us — forward with TTL decrement
    uint8_t ttl = PKT_TTL(hdr.flags);
    if (ttl > 0) {
        // Modify TTL in the RX buffer directly before forwarding
        uint8_t *rx_buf = rings[ring_id].rx_buf[rings[ring_id].rx_active_buf];
        rx_buf[3] = PKT_MAKE_FLAGS(ttl - 1, PKT_PRIORITY(hdr.flags));
        ring_forward(ring_id, ring_id);  // Same ring, downstream
    }

    ring_rx_done(ring_id);
    return false;
}
