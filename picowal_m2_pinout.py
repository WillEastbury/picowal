#!/usr/bin/env python3
"""picowal_m2_pinout.py -- M.2 Key-E custom pin allocation for PicoWAL family

This defines the standard pinout used across Mini and Midi backplanes.
Uses M.2 Key-E mechanical format (75 positions, key notch at pins 24-31).
Custom electrical allocation -- NOT compatible with standard WiFi/BT M.2 cards.

Physical: 22mm wide, lengths 2230/2242/2280 depending on card type.
"""

# M.2 Key-E has pins 1-75 with key notch removing pins 24-31 (8 pins missing)
# That leaves 67 usable pin positions.
# Pins are numbered from the connector edge, odd=top, even=bottom.

# Our custom PicoWAL Key-E allocation:
M2_KEYE_PINOUT = {
    # Power (near edge, first pins)
    1: "GND",
    2: "3V3",
    3: "GND",
    4: "3V3",
    5: "GND",
    6: "3V3",  # 3× 3.3V pairs, 2A max total

    # PCIe x1 (for midi FPGA SerDes link)
    7: "PCIE_TX_P",
    8: "PCIE_TX_N",
    9: "GND",
    10: "PCIE_RX_P",
    11: "PCIE_RX_N",
    12: "GND",
    13: "PCIE_REFCLK_P",
    14: "PCIE_REFCLK_N",
    15: "PCIE_RST_N",
    16: "PCIE_WAKE_N",

    # SPI bus (main data path for MCU tiers)
    17: "SPI_CLK",
    18: "SPI_MOSI",
    19: "SPI_MISO",
    20: "SPI_CS_N",
    21: "SPI_IRQ",  # card-to-host interrupt
    22: "SPI_RDY",  # card ready signal
    23: "GND",

    # Key notch: pins 24-31 removed (Key-E)

    # I2C management bus
    32: "I2C_SCL",
    33: "I2C_SDA",
    34: "GND",

    # UART debug
    35: "UART_TX",  # card -> host
    36: "UART_RX",  # host -> card

    # GPIO (directly mapped, directly usable)
    37: "GPIO0",   # card-detect (active low, internal pullup on backplane)
    38: "GPIO1",   # interrupt to host
    39: "GPIO2",   # reset from host (active low)
    40: "GPIO3",   # status LED output
    41: "GPIO4",   # general purpose
    42: "GPIO5",   # general purpose
    43: "GPIO6",   # general purpose
    44: "GPIO7",   # general purpose
    45: "GND",

    # SDIO / additional SPI (optional, tier-dependent)
    46: "SDIO_CLK",
    47: "SDIO_CMD",
    48: "SDIO_D0",
    49: "SDIO_D1",
    50: "SDIO_D2",
    51: "SDIO_D3",
    52: "GND",

    # Extended data bus (8-bit parallel, for higher throughput on MCU tiers)
    53: "PBUS_D0",
    54: "PBUS_D1",
    55: "PBUS_D2",
    56: "PBUS_D3",
    57: "PBUS_D4",
    58: "PBUS_D5",
    59: "PBUS_D6",
    60: "PBUS_D7",
    61: "PBUS_RDY",
    62: "PBUS_ACK",
    63: "PBUS_DIR",
    64: "GND",

    # Additional power + reserved
    65: "3V3",
    66: "GND",
    67: "RSVD0",
    68: "RSVD1",
    69: "RSVD2",
    70: "RSVD3",
    71: "GND",
    72: "3V3",
    73: "GND",
    74: "3V3",
    75: "GND",
}

# Standard M.2 M-key is used AS-IS for NVMe storage (no custom pinout needed)
# Off-the-shelf NVMe SSDs plug directly into M-key sockets on midi/maxi backplanes.

# Card type definitions
CARD_TYPES = {
    "cpu_pico": {
        "key": "E",
        "size": "2230",
        "desc": "RP2354B single MCU (Pico tier CPU)",
        "uses": ["SPI", "I2C", "UART", "GPIO"],
    },
    "cpu_mini": {
        "key": "E",
        "size": "2242",
        "desc": "2× RP2354B + PSRAM (Mini tier CPU)",
        "uses": ["SPI", "PBUS", "I2C", "UART", "GPIO"],
    },
    "cpu_midi": {
        "key": "E",
        "size": "2280",
        "desc": "ECP5UM5G-25F + SRAM + DPRAM (Midi tier CPU)",
        "uses": ["PCIE", "SPI", "I2C", "UART", "GPIO"],
    },
    "nic_w5500": {
        "key": "E",
        "size": "2230",
        "desc": "WIZnet W5500 100Mbps (Pico/Mini NIC)",
        "uses": ["SPI", "GPIO"],
    },
    "nic_w6100": {
        "key": "E",
        "size": "2230",
        "desc": "WIZnet W6100 GbE HW TCP/IP (Mini NIC)",
        "uses": ["SPI", "GPIO"],
    },
    "nic_rtl8221b": {
        "key": "E",
        "size": "2242",
        "desc": "RTL8221B 2.5GbE (Midi NIC, SGMII via PCIe SerDes)",
        "uses": ["PCIE", "I2C", "GPIO"],
    },
    "storage_sd": {
        "key": "E",
        "size": "2230",
        "desc": "SD card slot (Pico storage, via SDIO pins)",
        "uses": ["SDIO", "GPIO"],
    },
    "storage_emmc": {
        "key": "E",
        "size": "2242",
        "desc": "32GB eMMC (Mini storage, via SDIO pins)",
        "uses": ["SDIO", "I2C", "GPIO"],
    },
    "storage_nvme": {
        "key": "M",
        "size": "2280",
        "desc": "Standard NVMe SSD (Midi/Maxi, M-key passthrough)",
        "uses": ["PCIE_x4"],
    },
    "power_usbc": {
        "key": "E",
        "size": "2230",
        "desc": "USB-C PD power input (5-20V, negotiated)",
        "uses": ["GPIO"],  # PD negotiation via I2C on-card
    },
    "power_poe_af": {
        "key": "E",
        "size": "2230",
        "desc": "PoE 802.3af PD (13W)",
        "uses": ["GPIO"],
    },
    "power_poe_at": {
        "key": "E",
        "size": "2242",
        "desc": "PoE 802.3at PD (25.5W)",
        "uses": ["GPIO"],
    },
}


def print_pinout():
    """Print the full M.2 Key-E pinout table."""
    print("PicoWAL M.2 Key-E Custom Pinout (67 usable pins)")
    print("=" * 50)
    for pin in sorted(M2_KEYE_PINOUT.keys()):
        print(f"  Pin {pin:2d}: {M2_KEYE_PINOUT[pin]}")
    print()
    print("Card Types:")
    print("-" * 50)
    for name, info in CARD_TYPES.items():
        print(f"  {name:16s}  Key-{info['key']} {info['size']}  {info['desc']}")


if __name__ == "__main__":
    print_pinout()
