#include "ring.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#include "ring_uart.pio.h"

#include <string.h>

// ============================================================
// Half-duplex single-wire link — no addressing, no routing
// Send down the pin. Wait for reply. Done.
// ============================================================

#define MAX_PORTS 12
static uint8_t rx_bufs[MAX_PORTS][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];
static uint8_t tx_bufs[MAX_PORTS][PKT_MAX_PAYLOAD + PKT_HEADER_SIZE];

static PIO get_pio(uint8_t idx) {
    if (idx == 0) return pio0;
    if (idx == 1) return pio1;
    return pio2;
}

static uint tx_offsets[3] = {0xFFFF, 0xFFFF, 0xFFFF};
static uint rx_offsets[3] = {0xFFFF, 0xFFFF, 0xFFFF};

static void ensure_programs(uint8_t pio_idx) {
    if (tx_offsets[pio_idx] == 0xFFFF) {
        PIO pio = get_pio(pio_idx);
        tx_offsets[pio_idx] = pio_add_program(pio, &ring_tx_program);
        rx_offsets[pio_idx] = pio_add_program(pio, &ring_rx_program);
    }
}

static void switch_to_tx(link_port_t *port) {
    PIO pio = get_pio(port->pio_idx);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);

    pio_sm_set_enabled(pio, port->sm, false);
    pio_sm_set_consecutive_pindirs(pio, port->sm, port->pin, 1, true);

    pio_sm_config cfg = ring_tx_program_get_default_config(tx_offsets[port->pio_idx]);
    sm_config_set_sideset_pins(&cfg, port->pin);
    sm_config_set_out_shift(&cfg, false, true, 32);
    sm_config_set_clkdiv(&cfg, div);
    pio_sm_init(pio, port->sm, tx_offsets[port->pio_idx], &cfg);
    pio_sm_set_enabled(pio, port->sm, true);
}

static void switch_to_rx(link_port_t *port) {
    PIO pio = get_pio(port->pio_idx);
    float div = (float)clock_get_hz(clk_sys) / (2.0f * LINK_BAUD_RATE);

    pio_sm_set_enabled(pio, port->sm, false);
    pio_sm_set_consecutive_pindirs(pio, port->sm, port->pin, 1, false);

    pio_sm_config cfg = ring_rx_program_get_default_config(rx_offsets[port->pio_idx]);
    sm_config_set_in_pins(&cfg, port->pin);
    sm_config_set_in_shift(&cfg, false, true, 32);
    sm_config_set_clkdiv(&cfg, div);
    pio_sm_init(pio, port->sm, rx_offsets[port->pio_idx], &cfg);
    pio_sm_set_enabled(pio, port->sm, true);
}

// ============================================================
// Init
// ============================================================

void link_init_port(link_port_t *port, bool start_as_listener) {
    PIO pio = get_pio(port->pio_idx);
    ensure_programs(port->pio_idx);
    pio_gpio_init(pio, port->pin);

    if (start_as_listener) {
        switch_to_rx(port);
    } else {
        switch_to_tx(port);
    }
}

void link_init_ports(link_port_t *ports, uint8_t count,
                     uint8_t pin_base, uint8_t pio_idx, bool start_as_listener) {
    for (uint8_t i = 0; i < count; i++) {
        ports[i] = (link_port_t){
            .pin = pin_base + i,
            .sm = i,
            .pio_idx = pio_idx,
            .dma_ch = i,
        };
        link_init_port(&ports[i], start_as_listener);
    }
}

// ============================================================
// Send (switches to TX, sends, switches back to RX)
// ============================================================

void link_send(link_port_t *port, const pkt_header_t *hdr,
               const uint8_t *payload, uint16_t len) {
    PIO pio = get_pio(port->pio_idx);
    uint8_t *buf = tx_bufs[port->sm];

    // Frame: header + payload
    memcpy(buf, hdr, PKT_HEADER_SIZE);
    if (payload && len > 0) {
        memcpy(buf + PKT_HEADER_SIZE, payload, len);
    }
    uint32_t total = PKT_HEADER_SIZE + len;

    // Switch to TX, push data
    switch_to_tx(port);

    for (uint32_t i = 0; i < total; i += 4) {
        uint32_t word = 0;
        for (int b = 0; b < 4 && (i + b) < total; b++) {
            word |= ((uint32_t)buf[i + b]) << (24 - b * 8);
        }
        pio_sm_put_blocking(pio, port->sm, word);
    }

    // Wait for TX drain
    while (!pio_sm_is_tx_fifo_empty(pio, port->sm)) {
        tight_loop_contents();
    }

    // Turnaround + switch back to RX
    sleep_us(LINK_TURNAROUND_US);
    switch_to_rx(port);
}

// ============================================================
// Poll (check if anything arrived)
// ============================================================

bool link_poll(link_port_t *port, pkt_header_t *hdr, uint8_t **payload) {
    PIO pio = get_pio(port->pio_idx);

    if (pio_sm_is_rx_fifo_empty(pio, port->sm)) return false;

    uint8_t *buf = rx_bufs[port->sm];
    uint32_t idx = 0;

    // Drain FIFO
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

    // CRC check
    if (hdr->payload_len > 0 && hdr->payload_len <= PKT_MAX_PAYLOAD) {
        if (crc16_ccitt(*payload, hdr->payload_len) != hdr->crc16) return false;
    }

    return true;
}

// ============================================================
// Transact (send + wait for reply)
// ============================================================

bool link_transact(link_port_t *port,
                   const pkt_header_t *send_hdr, const uint8_t *send_payload, uint16_t send_len,
                   pkt_header_t *reply_hdr, uint8_t **reply_payload,
                   uint32_t timeout_us) {
    link_send(port, send_hdr, send_payload, send_len);

    uint64_t deadline = time_us_64() + timeout_us;
    while (time_us_64() < deadline) {
        if (link_poll(port, reply_hdr, reply_payload)) return true;
        sleep_us(1);
    }
    return false;
}

// ============================================================
// Poll any port
// ============================================================

int8_t link_poll_any(link_port_t *ports, uint8_t count,
                     pkt_header_t *hdr, uint8_t **payload) {
    for (uint8_t i = 0; i < count; i++) {
        if (link_poll(&ports[i], hdr, payload)) return i;
    }
    return -1;
}
