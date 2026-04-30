#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""generate_pico.py -- PicoWAL Pico Tier (Integrated, Single Board)

The smallest PicoWAL node: a single RP2354B running the full WAL query engine
in firmware, with W5500 hardware TCP/IP and SD card storage.

  - 1× RP2354B (dual M33 + dual RISC-V, 150MHz)
  - 1× WIZnet W5500 (100Mbps Ethernet, hardware TCP/IP, 8 sockets)
  - 1× SD card slot (via SPI, up to 128GB SDXC)
  - 1× 8MB LY68L6400 PSRAM (page cache)
  - Power: USB-C (5V 2A) or PoE 802.3af (13W) -- jumper selectable
  - Board: 40×30mm, 2-layer (hand-solderable except RP2354B QFN)

This is the entry-level dev/demo board. Same WAL protocol, same numeric
namespace (card/folder/file), just much slower (limited by 100Mbps + SD).

Performance targets:
  - SD sequential read: ~25 MB/s (SPI @ 50MHz)
  - Network: 12.5 MB/s (100Mbps line rate)
  - Query throughput: ~50K QPS cached (PSRAM), ~2K QPS cold (SD)
  - Concurrent connections: 8 (W5500 hardware limit)

Target cost: £30-50 all-in.
"""

import os, uuid

VERSION = "1.0"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "picowal_pico")

_uuid_counter = 0
def make_uuid():
    global _uuid_counter
    _uuid_counter += 1
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"picowal_pico.{_uuid_counter}"))


# ═══════════════════════════════════════════════════════════════════════
# BOM
# ═══════════════════════════════════════════════════════════════════════

PICO_BOM = [
    {"ref": "U1",  "value": "RP2354B",       "pkg": "QFN-60",    "cost": 1.40, "desc": "MCU: 2xM33+2xRV32, 150MHz, 520KB SRAM"},
    {"ref": "U2",  "value": "W5500",         "pkg": "QFP-48",    "cost": 2.20, "desc": "100Mbps Ethernet + HW TCP/IP (8 sockets)"},
    {"ref": "U3",  "value": "LY68L6400",     "pkg": "SOP-8",     "cost": 0.80, "desc": "8MB PSRAM (page cache, SPI/QPI)"},
    {"ref": "U4",  "value": "W25Q128JVSIQ",  "pkg": "SOIC-8",    "cost": 1.20, "desc": "16MB flash (firmware + config)"},
    {"ref": "U5",  "value": "TPS23753A",     "pkg": "TSSOP-20",  "cost": 2.50, "desc": "PoE 802.3af PD (13W, optional)"},
    {"ref": "U6",  "value": "TPS62A02",      "pkg": "SOT-23-6",  "cost": 0.90, "desc": "3.3V -> 1.1V buck (RP2354B core)"},
    {"ref": "U7",  "value": "AP2112K-3.3",   "pkg": "SOT-23-5",  "cost": 0.15, "desc": "5V -> 3.3V LDO (from USB-C or PoE)"},
    {"ref": "J1",  "value": "USB_C_PWR",     "pkg": "USB-C-16P", "cost": 0.30, "desc": "USB-C power input (5V 2A, no data)"},
    {"ref": "J2",  "value": "RJ45_MAGJACK",  "pkg": "RJ45",      "cost": 2.50, "desc": "RJ45 with integrated magnetics"},
    {"ref": "J3",  "value": "SD_SLOT",       "pkg": "SD-push",   "cost": 0.60, "desc": "Micro SD card slot"},
    {"ref": "J4",  "value": "CONN_1x4",      "pkg": "1x4-2.54",  "cost": 0.10, "desc": "UART debug header"},
    {"ref": "J5",  "value": "CONN_1x3",      "pkg": "1x3-2.54",  "cost": 0.05, "desc": "SWD debug (SWDIO/SWCLK/GND)"},
    {"ref": "Y1",  "value": "25MHz",         "pkg": "3215",      "cost": 0.20, "desc": "W5500 crystal (25MHz)"},
    {"ref": "Y2",  "value": "12MHz",         "pkg": "3215",      "cost": 0.20, "desc": "RP2354B crystal (12MHz, PLL to 150MHz)"},
    {"ref": "D1",  "value": "LED_GREEN",     "pkg": "0402",      "cost": 0.02, "desc": "Power LED"},
    {"ref": "D2",  "value": "LED_YELLOW",    "pkg": "0402",      "cost": 0.02, "desc": "Activity LED"},
    {"ref": "D3",  "value": "LED_BLUE",      "pkg": "0402",      "cost": 0.02, "desc": "Link LED"},
    {"ref": "SW1", "value": "BOOT_SEL",      "pkg": "0402",      "cost": 0.05, "desc": "BOOTSEL button (firmware update)"},
]


# ═══════════════════════════════════════════════════════════════════════
# Pin allocation
# ═══════════════════════════════════════════════════════════════════════

def pin_budget_report():
    print("\n  Pin Budget -- RP2354B QFN-60 (30 GPIO available)")
    print("  " + "-" * 50)
    pins = [
        ("W5500 SPI (SCK/MOSI/MISO/CS/INT/RST)", 6),
        ("SD card SPI (SCK/MOSI/MISO/CS/DET)", 5),
        ("PSRAM QPI (CLK/CS/IO0-IO3)", 6),
        ("Flash QSPI (CLK/CS/IO0-IO3)", 6),
        ("UART debug (TX/RX)", 2),
        ("LEDs (PWR/ACT/LINK)", 3),
        ("BOOTSEL button", 1),
    ]
    total = 0
    for name, n in pins:
        print(f"    {name:44s} {n:2d}")
        total += n
    print(f"    {'-'*44} {'-'*2}")
    print(f"    {'TOTAL':44s} {total:2d} / 30  ({'OK' if total <= 30 else 'OVER'})")
    print(f"    Spare GPIO: {30 - total}")


def power_budget_report():
    print("\n  Power Budget -- USB-C 5V/2A (10W) or PoE 802.3af (13W)")
    print("  " + "-" * 50)
    rails = [
        ("RP2354B (1.1V core + 3.3V I/O)", 0.275),
        ("W5500 (3.3V, ~180mA active)", 0.594),
        ("LY68L6400 PSRAM (3.3V, 25mA)", 0.083),
        ("W25Q128 flash (3.3V, 25mA active)", 0.083),
        ("SD card (3.3V, ~100mA during read)", 0.330),
        ("RJ45 MagJack LEDs", 0.040),
        ("Board LEDs x3", 0.030),
        ("LDO/buck overhead", 0.150),
    ]
    total = 0
    for name, w in rails:
        if w > 0:
            print(f"    {name:44s} {w:.3f}W")
            total += w
    print(f"    {'-'*44} {'-'*5}")
    print(f"    {'TOTAL':44s} {total:.3f}W")
    print(f"    USB-C budget: 10W, margin: {10.0 - total:.3f}W (OK)")
    print(f"    PoE budget:   13W, margin: {13.0 - total:.3f}W (OK)")


# ═══════════════════════════════════════════════════════════════════════
# KiCad schematic generator (simplified)
# ═══════════════════════════════════════════════════════════════════════

def gen_sch():
    """Generate KiCad 8 schematic for pico board."""
    header = f"""(kicad_sch
  (version 20231120)
  (generator "picowal_pico_gen")
  (uuid "{make_uuid()}")
  (paper "A4")
  (title_block
    (title "PicoWAL Pico - Entry Level Node")
    (rev "{VERSION}")
    (comment 1 "RP2354B + W5500 + SD + PSRAM")
    (comment 2 "40x30mm, 2-layer, USB-C/PoE powered")
  )
"""
    # Simplified: just place symbols with net labels
    symbols = []
    for i, part in enumerate(PICO_BOM):
        x = 50 + (i % 4) * 40
        y = 50 + (i // 4) * 30
        symbols.append(f"""  (symbol
    (lib_id "{part['value']}")
    (at {x} {y})
    (uuid "{make_uuid()}")
    (property "Reference" "{part['ref']}" (at {x} {y-3} 0))
    (property "Value" "{part['value']}" (at {x} {y+3} 0))
  )""")

    footer = "\n)\n"
    return header + "\n".join(symbols) + footer


def gen_pcb():
    """Generate KiCad 8 PCB for pico board (40x30mm, 2-layer)."""
    board_w, board_h = 40, 30
    header = f"""(kicad_pcb
  (version 20231014)
  (generator "picowal_pico_gen")
  (general
    (thickness 1.6)
    (pcbplotparams)
  )
  (paper "A4")
  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (36 "B.SilkS" user)
    (37 "F.SilkS" user)
    (44 "Edge.Cuts" user)
  )
  (setup
    (grid_origin 0 0)
  )
"""
    # Board outline
    outline = f"""  (gr_rect (start 0 0) (end {board_w} {board_h}) (layer "Edge.Cuts") (width 0.1) (uuid "{make_uuid()}"))
"""
    # Place footprints
    footprints = []
    placements = {
        "U1": (20, 15, "QFN-60"),      # RP2354B center
        "U2": (8, 8, "QFP-48"),        # W5500 top-left
        "U3": (32, 8, "SOP-8"),        # PSRAM top-right
        "U4": (32, 22, "SOIC-8"),      # Flash bottom-right
        "J1": (20, 28, "USB-C"),       # USB-C bottom center
        "J2": (2, 20, "RJ45"),         # RJ45 left
        "J3": (38, 15, "SD"),          # SD right
    }
    for ref, (x, y, pkg) in placements.items():
        footprints.append(f"""  (footprint "{pkg}"
    (at {x} {y})
    (uuid "{make_uuid()}")
    (property "Reference" "{ref}" (at 0 -2 0) (layer "F.SilkS"))
  )""")

    footer = "\n)\n"
    return header + outline + "\n".join(footprints) + footer


# ═══════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    sch_path = os.path.join(OUTPUT_DIR, "picowal_pico.kicad_sch")
    with open(sch_path, "w") as f:
        f.write(gen_sch())
    print("[OK] picowal_pico.kicad_sch")

    pcb_path = os.path.join(OUTPUT_DIR, "picowal_pico.kicad_pcb")
    with open(pcb_path, "w") as f:
        f.write(gen_pcb())
    print("[OK] picowal_pico.kicad_pcb")

    # Reports
    print(f"\n{'='*60}")
    print(f" PicoWAL PICO v{VERSION} -- Entry Level Node")
    print(f"{'='*60}")
    print()
    print(" Single board, no backplane. Smallest possible PicoWAL node.")
    print()
    print("   RP2354B          2xM33 + 2xRV32 @ 150MHz, 520KB SRAM")
    print("   W5500            100Mbps Ethernet, HW TCP/IP (8 sockets)")
    print("   LY68L6400        8MB PSRAM (page cache + query working set)")
    print("   W25Q128          16MB flash (firmware + B-tree root cache)")
    print("   Micro SD         Up to 128GB SDXC (cold storage)")
    print("   Power            USB-C 5V/2A or PoE 802.3af (13W)")
    print("   Board            40x30mm, 2-layer")
    print()
    print(" Performance:")
    print("   Cached query:    ~50K QPS (PSRAM B-tree hit, 20us per query)")
    print("   Cold query:      ~2K QPS (SD read, 500us per 4KB block)")
    print("   Network:         12.5 MB/s (100Mbps line rate)")
    print("   Connections:     8 simultaneous (W5500 hardware limit)")
    print()
    print(" Firmware roles (both cores active):")
    print("   Core 0 (M33):   Network stack + query dispatch (W5500 driver)")
    print("   Core 1 (M33):   Query engine (B-tree walk, predicate filter)")
    print("   Core 2 (RV32):  WAL writer + index update")
    print("   Core 3 (RV32):  Background: compaction, PSRAM cache eviction")
    print()
    print(" Data paths:")
    print("   KV READ:  W5500 -> SPI -> RP2354 -> PSRAM lookup -> response")
    print("   KV WRITE: W5500 -> SPI -> RP2354 -> SD WAL append -> ack")
    print("   SCAN:     W5500 -> SPI -> RP2354 -> SD sequential -> filter -> response")
    print()
    print(" Storage layout (SD card):")
    print("   Sector 0-1023:       WAL (ring buffer, 512KB)")
    print("   Sector 1024-8191:    B-tree index (3.5MB)")
    print("   Sector 8192+:        Data pages (remaining space)")
    print("   No filesystem -- raw sector access via SPI")
    print()

    print(" BOM:")
    total = sum(p["cost"] for p in PICO_BOM) + 3.00 + 2.00
    for part in PICO_BOM:
        print(f"   {part['ref']:6s}  {part['value']:16s}  {chr(163)}{part['cost']:.2f}  {part['desc']}")
    print(f"   {'PCB':6s}  {'2-layer 40x30mm':16s}  {chr(163)}3.00")
    print(f"   {'MISC':6s}  {'Passives+bypass':16s}  {chr(163)}2.00")
    print(f"   {'-'*55}")
    print(f"   {'TOTAL':6s}  {'(board only)':16s}  {chr(163)}{total:.2f}")
    print()
    print(f" Full system cost:")
    sd_cost = 12.0
    print(f"   Board:                 {chr(163)}{total:.2f}")
    print(f"   128GB SD card:         {chr(163)}{sd_cost:.2f}")
    print(f"   Enclosure:             {chr(163)}5.00")
    grand = total + sd_cost + 5.0
    print(f"   {'-'*35}")
    print(f"   TOTAL:                 {chr(163)}{grand:.2f}")
    print()

    pin_budget_report()
    power_budget_report()
    print(f"\n{'='*60}")


if __name__ == "__main__":
    main()
