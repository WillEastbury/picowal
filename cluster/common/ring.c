#include "ring.h"
#include "../common/config.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "ring_uart.pio.h"  // Generated from .pio file

// ============================================================
// PIO Ring Driver — Manchester encoded serial rings with DMA
// ============================================================

ring_state_t rings[RING_COUNT];
static ring_snoop_cb g_snoop_cb = NULL;

// PIO instance lookup
static PIO pio_for_ring(uint8_t ring_id) {
    if (ring_id < 2) return pio0;
    return pio1;
}

// ============================================================
// Initialisation
// ============================================================

void ring_init(uint8_t ring_id, const ring_config_t *config) {
    ring_state_t *ring = &rings[ring_id];
    memset(ring, 0, sizeof(*ring));
    ring->config = *config;

    PIO pio = pio_for_ring(ring_id);
    uint sm_tx = config->sm_tx;
    uint sm_rx = config->sm_rx;

    // Load TX program
    uint tx_offset = pio_add_program(pio, &ring_tx_program);

    // Configure TX SM
    pio_sm_config tx_cfg = ring_tx_program_get_default_config(tx_offset);
    sm_config_set_sideset_pins(&tx_cfg, config->pin_tx);
    sm_config_set_out_shift(&tx_cfg, false, true, 32);  // Shift left, autopull, 32 bits
    // Clock divider: sys_clock / (2 * baud) for Manchester (2 cycles per bit)
    float div = (float)clock_get_hz(clk_sys) / (2.0f * config->baud_rate);
    sm_config_set_clkdiv(&tx_cfg, div);

    pio_gpio_init(pio, config->pin_tx);
    pio_sm_set_consecutive_pindirs(pio, sm_tx, config->pin_tx, 1, true);

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
