#include "ring.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#include "ring_uart.pio.h"

#include <string.h>

// ============================================================
// Half-duplex single-wire PIO link driver
// ============================================================
// Manchester encoded, master-initiated protocol.
// Single GPIO switches between output (TX) and input (RX).
// PIO SM handles encoding/decoding, DMA moves data.

// --- Buffers (per port, statically indexed by SM) ---
#define MAX_PORTS 12
static uint8_t rx_bufs[MAX_PORTS][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
static uint8_t tx_bufs[MAX_PORTS][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];

// PIO block lookup
static PIO get_pio(uint8_t idx) {
    if (idx == 0) return pio0;
    if (idx == 1) return pio1;
    return pio2;
}

// --- Direction control ---
static void set_pin_output(link_port_t *port) {
    PIO pio = get_pio(port->pio_idx);
    pio_sm_set_consecutive_pindirs(pio, port->sm, port->pin, 1, true);
}

static void set_pin_input(link_port_t *port) {
    PIO pio = get_pio(port->pio_idx);
    pio_sm_set_consecutive_pindirs(pio, port->sm, port->pin, 1, false);
}

// --- PIO program loading (cached per block) ---
static uint tx_offsets[3] = {0xFFFF, 0xFFFF, 0xFFFF};
static uint rx_offsets[3] = {0xFFFF, 0xFFFF, 0xFFFF};

static void ensure_programs_loaded(uint8_t pio_idx) {
    PIO pio = get_pio(pio_idx);
    if (tx_offsets[pio_idx] == 0xFFFF) {
        tx_offsets[pio_idx] = pio_add_program(pio, &ring_tx_program);
        rx_offsets[pio_idx] = pio_add_program(pio, &ring_rx_program);
    }
}

// ============================================================
// Port initialization
// ============================================================

void link_init_port(link_port_t *port) {
    PIO pio = get_pio(port->pio_idx);
    ensure_programs_loaded(port->pio_idx);

    pio_gpio_init(pio, port->pin);

    // Start in appropriate mode
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);

    if (port->is_master) {
        // Master starts idle (output low)
        set_pin_output(port);
        pio_sm_config cfg = ring_tx_program_get_default_config(tx_offsets[port->pio_idx]);
        sm_config_set_sideset_pins(&cfg, port->pin);
        sm_config_set_out_shift(&cfg, false, true, 32);
        sm_config_set_clkdiv(&cfg, div);
        pio_sm_init(pio, port->sm, tx_offsets[port->pio_idx], &cfg);
        pio_sm_set_enabled(pio, port->sm, true);
    } else {
        // Slave starts listening (input)
        set_pin_input(port);
        pio_sm_config cfg = ring_rx_program_get_default_config(rx_offsets[port->pio_idx]);
        sm_config_set_in_pins(&cfg, port->pin);
        sm_config_set_in_shift(&cfg, false, true, 32);
        sm_config_set_clkdiv(&cfg, div);
        pio_sm_init(pio, port->sm, rx_offsets[port->pio_idx], &cfg);
        pio_sm_set_enabled(pio, port->sm, true);
    }
}

void link_init_ports(link_port_t *ports, uint8_t count,
                     uint8_t pin_base, uint8_t pio_idx, bool is_master) {
    for (uint8_t i = 0; i < count; i++) {
        ports[i].pin = pin_base + i;
        ports[i].sm = i;  // SM 0..N
        ports[i].pio_idx = pio_idx;
        ports[i].dma_ch = i;
        ports[i].is_master = is_master;
        link_init_port(&ports[i]);
    }
}

// ============================================================
// Master API
// ============================================================

bool link_master_send(link_port_t *port, const pkt_header_t *hdr,
                      const uint8_t *payload, uint16_t len) {
    PIO pio = get_pio(port->pio_idx);
    uint8_t *buf = tx_bufs[port->sm];

    // Assemble frame
    memcpy(buf, hdr, PKT_HEADER_SIZE);
    if (payload && len > 0) {
        memcpy(buf + PKT_HEADER_SIZE, payload, len);
    }
    uint32_t total = PKT_HEADER_SIZE + len;

    // Ensure TX mode
    pio_sm_set_enabled(pio, port->sm, false);
    set_pin_output(port);
    pio_sm_config cfg = ring_tx_program_get_default_config(tx_offsets[port->pio_idx]);
    sm_config_set_sideset_pins(&cfg, port->pin);
    sm_config_set_out_shift(&cfg, false, true, 32);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);
    sm_config_set_clkdiv(&cfg, div);
    pio_sm_init(pio, port->sm, tx_offsets[port->pio_idx], &cfg);
    pio_sm_set_enabled(pio, port->sm, true);

    // DMA transfer
    dma_channel_config c = dma_channel_get_default_config(port->dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, port->sm, true));

    dma_channel_configure(port->dma_ch, &c,
                          &pio->txf[port->sm], buf, total, true);
    dma_channel_wait_for_finish_blocking(port->dma_ch);

    // Turnaround: switch to RX mode
    sleep_us(LINK_TURNAROUND_US);
    pio_sm_set_enabled(pio, port->sm, false);
    set_pin_input(port);
    pio_sm_config rx_cfg = ring_rx_program_get_default_config(rx_offsets[port->pio_idx]);
    sm_config_set_in_pins(&rx_cfg, port->pin);
    sm_config_set_in_shift(&rx_cfg, false, true, 32);
    sm_config_set_clkdiv(&rx_cfg, div);
    pio_sm_init(pio, port->sm, rx_offsets[port->pio_idx], &rx_cfg);
    pio_sm_set_enabled(pio, port->sm, true);

    // Start RX DMA
    uint8_t *rxbuf = rx_bufs[port->sm];
    dma_channel_config rc = dma_channel_get_default_config(port->dma_ch);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_8);
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    channel_config_set_dreq(&rc, pio_get_dreq(pio, port->sm, false));

    dma_channel_configure(port->dma_ch, &rc,
                          rxbuf, &pio->rxf[port->sm],
                          PKT_MAX_PAYLOAD + PKT_HEADER_SIZE, true);

    return true;
}

bool link_master_poll_reply(link_port_t *port, pkt_header_t *hdr, uint8_t **payload) {
    if (dma_channel_is_busy(port->dma_ch)) return false;

    uint8_t *buf = rx_bufs[port->sm];
    memcpy(hdr, buf, PKT_HEADER_SIZE);
    *payload = buf + PKT_HEADER_SIZE;

    // Verify CRC
    if (hdr->payload_len > 0 && hdr->payload_len <= PKT_MAX_PAYLOAD) {
        uint16_t crc = crc16_ccitt(*payload, hdr->payload_len);
        if (crc != hdr->crc16) return false;
    }

    return true;
}

bool link_master_transact(link_port_t *port, const pkt_header_t *hdr,
                          const uint8_t *payload, uint16_t len,
                          pkt_header_t *reply_hdr, uint8_t **reply_payload,
                          uint32_t timeout_us) {
    link_master_send(port, hdr, payload, len);

    uint64_t deadline = time_us_64() + timeout_us;
    while (time_us_64() < deadline) {
        if (link_master_poll_reply(port, reply_hdr, reply_payload)) {
            return true;
        }
        sleep_us(1);
    }
    return false;
}

// ============================================================
// Slave API
// ============================================================

bool link_slave_poll_rx(link_port_t *port, pkt_header_t *hdr, uint8_t **payload) {
    PIO pio = get_pio(port->pio_idx);

    // Check if RX FIFO has data
    if (pio_sm_is_rx_fifo_empty(pio, port->sm)) return false;

    // Read into buffer via DMA (or direct if small)
    uint8_t *buf = rx_bufs[port->sm];

    // Simple: drain FIFO into buffer
    uint32_t idx = 0;
    while (!pio_sm_is_rx_fifo_empty(pio, port->sm) &&
           idx < PKT_MAX_PAYLOAD + PKT_HEADER_SIZE) {
        uint32_t word = pio_sm_get(pio, port->sm);
        buf[idx++] = (word >> 24) & 0xFF;
        buf[idx++] = (word >> 16) & 0xFF;
        buf[idx++] = (word >> 8) & 0xFF;
        buf[idx++] = word & 0xFF;
    }

    if (idx < PKT_HEADER_SIZE) return false;

    memcpy(hdr, buf, PKT_HEADER_SIZE);
    *payload = buf + PKT_HEADER_SIZE;

    // Verify
    if (hdr->payload_len > 0) {
        uint16_t crc = crc16_ccitt(*payload, hdr->payload_len);
        if (crc != hdr->crc16) return false;
    }

    return true;
}

void link_slave_reply(link_port_t *port, const pkt_header_t *hdr,
                      const uint8_t *payload, uint16_t len) {
    PIO pio = get_pio(port->pio_idx);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);

    // Switch to TX
    pio_sm_set_enabled(pio, port->sm, false);
    set_pin_output(port);
    pio_sm_config cfg = ring_tx_program_get_default_config(tx_offsets[port->pio_idx]);
    sm_config_set_sideset_pins(&cfg, port->pin);
    sm_config_set_out_shift(&cfg, false, true, 32);
    sm_config_set_clkdiv(&cfg, div);
    pio_sm_init(pio, port->sm, tx_offsets[port->pio_idx], &cfg);
    pio_sm_set_enabled(pio, port->sm, true);

    // Assemble and send
    uint8_t *buf = tx_bufs[port->sm];
    memcpy(buf, hdr, PKT_HEADER_SIZE);
    if (payload && len > 0) {
        memcpy(buf + PKT_HEADER_SIZE, payload, len);
    }
    uint32_t total = PKT_HEADER_SIZE + len;

    // Push to PIO TX FIFO
    for (uint32_t i = 0; i < total; i += 4) {
        uint32_t word = 0;
        for (int b = 0; b < 4 && (i + b) < total; b++) {
            word |= ((uint32_t)buf[i + b]) << (24 - b * 8);
        }
        pio_sm_put_blocking(pio, port->sm, word);
    }

    // Wait for TX to drain
    while (!pio_sm_is_tx_fifo_empty(pio, port->sm)) {
        tight_loop_contents();
    }
    sleep_us(LINK_TURNAROUND_US);

    // Switch back to RX
    pio_sm_set_enabled(pio, port->sm, false);
    set_pin_input(port);
    pio_sm_config rx_cfg = ring_rx_program_get_default_config(rx_offsets[port->pio_idx]);
    sm_config_set_in_pins(&rx_cfg, port->pin);
    sm_config_set_in_shift(&rx_cfg, false, true, 32);
    sm_config_set_clkdiv(&rx_cfg, div);
    pio_sm_init(pio, port->sm, rx_offsets[port->pio_idx], &rx_cfg);
    pio_sm_set_enabled(pio, port->sm, true);
}

// ============================================================
// Multi-port helpers
// ============================================================

int8_t link_poll_any(link_port_t *ports, uint8_t count,
                     pkt_header_t *hdr, uint8_t **payload) {
    for (uint8_t i = 0; i < count; i++) {
        if (ports[i].is_master) {
            if (link_master_poll_reply(&ports[i], hdr, payload)) return i;
        } else {
            if (link_slave_poll_rx(&ports[i], hdr, payload)) return i;
        }
    }
    return -1;
}
