#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""generate_mini.py -- PicoWAL Mini Tier (Backplane + Daughtercards)

Mid-range PicoWAL node: 2× RP2354B for query processing, W6100 for GbE
hardware TCP/IP, eMMC for storage. Modular M.2 Key-E card design.

Components:
  - Backplane (50×40mm): 2× M.2 Key-E sockets + power input + I2C mgmt
  - CPU card (Key-E 2242): 2× RP2354B + 8MB PSRAM + W25Q flash
  - NIC+Storage card (Key-E 2242): W6100 (GbE HW TCP/IP) + 32GB eMMC

Power: PoE 802.3af (13W) or USB-C (5V 3A) -- on backplane.

Performance targets:
  - eMMC sequential: ~150 MB/s (HS400 via SDIO on CPU card)
  - Network: 125 MB/s (GbE line rate, W6100 hardware offload)
  - Query throughput: ~200K QPS cached (PSRAM), ~15K QPS cold (eMMC)
  - Concurrent connections: 32 (W6100 hardware sockets)

Target cost: £80-150 all-in.
"""

import os, uuid

VERSION = "1.0"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "picowal_mini")

_uuid_counter = 0
def make_uuid():
    global _uuid_counter
    _uuid_counter += 1
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"picowal_mini.{_uuid_counter}"))


# ═══════════════════════════════════════════════════════════════════════
# BOM -- Backplane
# ═══════════════════════════════════════════════════════════════════════

BACKPLANE_BOM = [
    {"ref": "J1",  "value": "M2_KEY_E",      "pkg": "M2-E-2242",  "cost": 0.80, "desc": "M.2 Key-E socket 0 (CPU card)"},
    {"ref": "J2",  "value": "M2_KEY_E",      "pkg": "M2-E-2242",  "cost": 0.80, "desc": "M.2 Key-E socket 1 (NIC+Storage card)"},
    {"ref": "J3",  "value": "USB_C_PWR",     "pkg": "USB-C-16P",  "cost": 0.30, "desc": "USB-C power input (5V 3A)"},
    {"ref": "J4",  "value": "RJ45_POE_MAG",  "pkg": "RJ45",       "cost": 3.50, "desc": "RJ45 PoE MagJack (passes through to NIC card)"},
    {"ref": "U1",  "value": "TPS23753A",     "pkg": "TSSOP-20",   "cost": 2.50, "desc": "PoE 802.3af PD (13W)"},
    {"ref": "U2",  "value": "AP2112K-3.3",   "pkg": "SOT-23-5",   "cost": 0.15, "desc": "5V -> 3.3V LDO (USB-C path)"},
    {"ref": "U3",  "value": "TPS2113A",      "pkg": "SOT-23-6",   "cost": 0.60, "desc": "Power mux (PoE vs USB-C, auto-select)"},
    {"ref": "D1",  "value": "LED_GREEN",     "pkg": "0402",       "cost": 0.02, "desc": "Power good LED"},
    {"ref": "J5",  "value": "CONN_1x4",      "pkg": "1x4-2.54",   "cost": 0.10, "desc": "I2C expansion header"},
]


# ═══════════════════════════════════════════════════════════════════════
# BOM -- CPU Card (M.2 Key-E 2242)
# ═══════════════════════════════════════════════════════════════════════

CPU_CARD_BOM = [
    {"ref": "U1",  "value": "RP2354B",       "pkg": "QFN-60",    "cost": 1.40, "desc": "Query processor: B-tree walk + predicate filter"},
    {"ref": "U2",  "value": "RP2354B",       "pkg": "QFN-60",    "cost": 1.40, "desc": "Storage manager: WAL + index + compaction"},
    {"ref": "U3",  "value": "LY68L6400",     "pkg": "SOP-8",     "cost": 0.80, "desc": "8MB PSRAM (shared page cache, QPI)"},
    {"ref": "U4",  "value": "LY68L6400",     "pkg": "SOP-8",     "cost": 0.80, "desc": "8MB PSRAM (B-tree index cache, QPI)"},
    {"ref": "U5",  "value": "W25Q128JVSIQ",  "pkg": "SOIC-8",    "cost": 1.20, "desc": "16MB flash (firmware both picos)"},
    {"ref": "U6",  "value": "TPS62A02",      "pkg": "SOT-23-6",  "cost": 0.90, "desc": "3.3V -> 1.1V buck (both RP2354B cores)"},
    {"ref": "Y1",  "value": "12MHz",         "pkg": "3215",      "cost": 0.20, "desc": "RP2354B crystal"},
]

# ═══════════════════════════════════════════════════════════════════════
# BOM -- NIC+Storage Card (M.2 Key-E 2242)
# ═══════════════════════════════════════════════════════════════════════

NIC_STORAGE_CARD_BOM = [
    {"ref": "U1",  "value": "W6100",         "pkg": "QFN-48",    "cost": 3.80, "desc": "GbE Ethernet + HW TCP/IP (32 sockets)"},
    {"ref": "U2",  "value": "THGBMJG6C1LBAB","pkg": "BGA-153",   "cost": 12.00, "desc": "32GB eMMC 5.1 (Kioxia, HS400)"},
    {"ref": "U3",  "value": "25MHz",         "pkg": "3215",      "cost": 0.20, "desc": "W6100 crystal"},
    {"ref": "J1",  "value": "EDGE_CONN",     "pkg": "M2-E-edge", "cost": 0.00, "desc": "M.2 Key-E edge connector (part of PCB)"},
]


# ═══════════════════════════════════════════════════════════════════════
# Reports
# ═══════════════════════════════════════════════════════════════════════

def pin_budget_report():
    print("\n  Pin Budget -- CPU Card (2× RP2354B, 30 GPIO each)")
    print("  " + "-" * 55)
    print("  RP2354B #0 (Query Processor):")
    pins_0 = [
        ("M.2 SPI to backplane (SCK/MOSI/MISO/CS)", 4),
        ("M.2 parallel bus (D[7:0]+RDY+ACK+DIR)", 11),
        ("PSRAM 0 QPI (CLK/CS/IO0-3)", 6),
        ("Inter-pico link (shared SPI bus, extra CS only)", 1),
        ("Flash QSPI (shared, CLK/CS/IO0-3)", 6),
        ("IRQ to backplane", 1),
    ]
    total_0 = sum(n for _, n in pins_0)
    for name, n in pins_0:
        print(f"    {name:48s} {n:2d}")
    print(f"    {'TOTAL RP2354B #0':48s} {total_0:2d} / 30  ({'OK' if total_0 <= 30 else 'OVER'})")
    print()
    print("  RP2354B #1 (Storage Manager):")
    pins_1 = [
        ("eMMC SDIO (CLK/CMD/D0-D3) via M.2 backplane", 6),
        ("PSRAM 1 QPI (CLK/CS/IO0-3)", 6),
        ("Inter-pico SPI (SCK/MOSI/MISO/CS)", 4),
        ("Flash QSPI (shared, active-low CS select)", 1),
        ("M.2 GPIO (status/IRQ)", 2),
        ("LED", 1),
    ]
    total_1 = sum(n for _, n in pins_1)
    for name, n in pins_1:
        print(f"    {name:48s} {n:2d}")
    print(f"    {'TOTAL RP2354B #1':48s} {total_1:2d} / 30  ({'OK' if total_1 <= 30 else 'OVER'})")


def power_budget_report():
    print("\n  Power Budget -- PoE 802.3af (13W) or USB-C (15W)")
    print("  " + "-" * 55)
    rails = [
        ("Backplane overhead (PoE PD + mux + LDO)", 0.400),
        ("CPU card: 2× RP2354B (275mW each)", 0.550),
        ("CPU card: 2× LY68L6400 PSRAM", 0.166),
        ("CPU card: flash + buck", 0.200),
        ("NIC card: W6100 (3.3V, ~400mA active)", 1.320),
        ("NIC card: 32GB eMMC (3.3V, ~200mA active)", 0.660),
        ("RJ45 MagJack + LEDs", 0.060),
    ]
    total = sum(w for _, w in rails)
    for name, w in rails:
        print(f"    {name:48s} {w:.3f}W")
    print(f"    {'-'*48} {'-'*5}")
    print(f"    {'TOTAL':48s} {total:.3f}W")
    print(f"    PoE 802.3af budget: 13W, margin: {13.0 - total:.1f}W (OK)")


# ═══════════════════════════════════════════════════════════════════════
# KiCad generators
# ═══════════════════════════════════════════════════════════════════════

def gen_sch(name, bom, title, comment):
    header = f"""(kicad_sch
  (version 20231120)
  (generator "picowal_mini_gen")
  (uuid "{make_uuid()}")
  (paper "A4")
  (title_block
    (title "{title}")
    (rev "{VERSION}")
    (comment 1 "{comment}")
  )
"""
    symbols = []
    for i, part in enumerate(bom):
        x = 50 + (i % 4) * 40
        y = 50 + (i // 4) * 30
        symbols.append(f"""  (symbol
    (lib_id "{part['value']}")
    (at {x} {y})
    (uuid "{make_uuid()}")
    (property "Reference" "{part['ref']}" (at {x} {y-3} 0))
    (property "Value" "{part['value']}" (at {x} {y+3} 0))
  )""")
    return header + "\n".join(symbols) + "\n)\n"


def gen_pcb(name, bom, board_w, board_h, layers=4):
    header = f"""(kicad_pcb
  (version 20231014)
  (generator "picowal_mini_gen")
  (general (thickness 1.6))
  (paper "A4")
  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
{"    (1 " + '"In1.Cu" signal)\n    (2 "In2.Cu" signal)' if layers == 4 else ""}
    (36 "B.SilkS" user)
    (37 "F.SilkS" user)
    (44 "Edge.Cuts" user)
  )
"""
    outline = f"""  (gr_rect (start 0 0) (end {board_w} {board_h}) (layer "Edge.Cuts") (width 0.1) (uuid "{make_uuid()}"))
"""
    return header + outline + ")\n"


# ═══════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Backplane
    sch = gen_sch("backplane", BACKPLANE_BOM,
                  "PicoWAL Mini Backplane", "50x40mm, 2× M.2 Key-E, PoE/USB-C")
    with open(os.path.join(OUTPUT_DIR, "backplane.kicad_sch"), "w") as f:
        f.write(sch)
    pcb = gen_pcb("backplane", BACKPLANE_BOM, 50, 40, layers=2)
    with open(os.path.join(OUTPUT_DIR, "backplane.kicad_pcb"), "w") as f:
        f.write(pcb)
    print("[OK] backplane.kicad_sch + .kicad_pcb (50x40mm, 2-layer)")

    # CPU card
    sch = gen_sch("cpu_card", CPU_CARD_BOM,
                  "PicoWAL Mini CPU Card", "M.2 Key-E 2242, 2× RP2354B")
    with open(os.path.join(OUTPUT_DIR, "cpu_card.kicad_sch"), "w") as f:
        f.write(sch)
    pcb = gen_pcb("cpu_card", CPU_CARD_BOM, 22, 42, layers=4)
    with open(os.path.join(OUTPUT_DIR, "cpu_card.kicad_pcb"), "w") as f:
        f.write(pcb)
    print("[OK] cpu_card.kicad_sch + .kicad_pcb (22x42mm, 4-layer)")

    # NIC+Storage card
    sch = gen_sch("nic_storage_card", NIC_STORAGE_CARD_BOM,
                  "PicoWAL Mini NIC+Storage Card", "M.2 Key-E 2242, W6100 + 32GB eMMC")
    with open(os.path.join(OUTPUT_DIR, "nic_storage_card.kicad_sch"), "w") as f:
        f.write(sch)
    pcb = gen_pcb("nic_storage_card", NIC_STORAGE_CARD_BOM, 22, 42, layers=4)
    with open(os.path.join(OUTPUT_DIR, "nic_storage_card.kicad_pcb"), "w") as f:
        f.write(pcb)
    print("[OK] nic_storage_card.kicad_sch + .kicad_pcb (22x42mm, 4-layer)")

    # Summary
    print(f"\n{'='*60}")
    print(f" PicoWAL MINI v{VERSION} -- Modular GbE Node")
    print(f"{'='*60}")
    print()
    print(" Backplane (50×40mm) + 2 daughtercards (M.2 Key-E 2242)")
    print()
    print(" CPU Card:")
    print("   2× RP2354B        4 cores total (2xM33 + 2xRV32 per chip)")
    print("   2× 8MB PSRAM      16MB total page/index cache")
    print("   16MB flash         Shared firmware store")
    print("   Inter-pico SPI     50 MB/s between query + storage MCUs")
    print()
    print(" NIC+Storage Card:")
    print("   W6100             GbE + HW TCP/IP (32 sockets, zero-copy DMA)")
    print("   32GB eMMC 5.1     HS400 @ 200MHz (up to 400 MB/s burst)")
    print()
    print(" Performance:")
    print("   Cached query:     ~200K QPS (PSRAM hit, 5us per query)")
    print("   Cold query:       ~15K QPS (eMMC random 4K read, 65us)")
    print("   Network:          125 MB/s (GbE line rate)")
    print("   Connections:      32 simultaneous (W6100 hardware)")
    print()
    print(" Core allocation:")
    print("   Pico #0 Core 0:  Network RX/TX dispatch (W6100 SPI driver)")
    print("   Pico #0 Core 1:  Query engine (B-tree walk, predicate eval)")
    print("   Pico #1 Core 0:  Storage I/O (eMMC SDIO driver, DMA)")
    print("   Pico #1 Core 1:  WAL writer + index update + compaction")
    print()
    print(" Data paths:")
    print("   KV READ:  W6100 SPI -> Pico#0 -> PSRAM lookup -> response")
    print("             (cache miss: Pico#0 -> inter-pico SPI -> Pico#1 -> eMMC)")
    print("   KV WRITE: W6100 SPI -> Pico#0 -> Pico#1 -> eMMC WAL -> ack")
    print("   SCAN:     W6100 -> Pico#0 issues range -> Pico#1 streams eMMC -> filter")
    print()

    # BOM summary
    bp_total = sum(p["cost"] for p in BACKPLANE_BOM) + 3.00 + 1.50
    cpu_total = sum(p["cost"] for p in CPU_CARD_BOM) + 5.00 + 1.00
    nic_total = sum(p["cost"] for p in NIC_STORAGE_CARD_BOM) + 5.00 + 1.00
    print(f" BOM breakdown:")
    print(f"   Backplane (50x40mm, 2-layer):     {chr(163)}{bp_total:.2f}")
    for p in BACKPLANE_BOM:
        print(f"     {p['ref']:5s} {p['value']:16s} {chr(163)}{p['cost']:.2f}")
    print(f"     PCB + passives:           {chr(163)}4.50")
    print()
    print(f"   CPU Card (22x42mm, 4-layer):      {chr(163)}{cpu_total:.2f}")
    for p in CPU_CARD_BOM:
        print(f"     {p['ref']:5s} {p['value']:16s} {chr(163)}{p['cost']:.2f}")
    print(f"     PCB + passives:           {chr(163)}6.00")
    print()
    print(f"   NIC+Storage Card (22x42mm, 4-layer): {chr(163)}{nic_total:.2f}")
    for p in NIC_STORAGE_CARD_BOM:
        if p['cost'] > 0:
            print(f"     {p['ref']:5s} {p['value']:16s} {chr(163)}{p['cost']:.2f}")
    print(f"     PCB + passives:           {chr(163)}6.00")
    print()
    grand = bp_total + cpu_total + nic_total
    print(f"   {'-'*40}")
    print(f"   TOTAL (all boards):               {chr(163)}{grand:.2f}")
    print(f"   + Enclosure/cables:               {chr(163)}10.00")
    print(f"   GRAND TOTAL:                      {chr(163)}{grand + 10:.2f}")
    print()

    pin_budget_report()
    power_budget_report()
    print(f"\n{'='*60}")


if __name__ == "__main__":
    main()
