#!/usr/bin/env python3
"""generate_picowal.py — PicoWAL Query Processor Card

Generates KiCad 8 schematic + PCB for a single PicoWAL query processor:

  - 1× iCE40HX8K-CT256: SPI bridge with bypass/query split
  - 1× RP2354B: query planning, decomposition, result aggregation
  - 2× IS61WV25616BLL: FPGA-owned page staging (512KB, parallel banks)
  - 1× LY68L6400: RP-owned 8MB PSRAM for working set / aggregation
  - 1× W25Q128: RP flash
  - 1× W25Q32: FPGA config flash
  - OC: TPS62A01 adjustable buck for DVDD (1.1-1.25V)

Key insight: indexes are stored as reserved card_id blocks on the primary
SATA KV store. No separate index card needed.

  Address space: [63:53]=tenant, [52:42]=card_id, [41:0]=block
  Reserved:      card_id 0x7FF = index pages (per-tenant)
                 card_id 0x7FE = WAL/journal pages
                 card_id 0x000-0x7FD = user data cards

PicoWAL query flow:
  FAST PATH (plain KV read/write):
    1. Packet arrives at FPGA via upstream SPI
    2. FPGA inspects cmd_flags — if bit 1 (QUERY) is clear → BYPASS
    3. FPGA forwards cmd directly to downstream SPI (SATA KV nodes)
    4. Zero RP involvement, zero copy, ~2 SPI clock overhead

  QUERY PATH (index lookup + multi-read):
    1. Packet arrives, FPGA sees QUERY flag set
    2. FPGA stages cmd in SRAM ring buffer, asserts IRQ to RP
    3. RP reads query descriptor from SRAM via FPGA SPI register map
    4. RP reads index pages from reserved card_id=0x7FF on SATA store
    5. RP decomposes into data page reads, dispatches via downstream SPI
    6. Results buffered in PSRAM, aggregated, returned via upstream SPI

Board: ~70×50mm, 4-layer, QFN/BGA/TSOP — requires reflow.
"""

import os, uuid

VERSION = "1.0"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "picowal")

_uuid_counter = 0
def make_uuid():
    global _uuid_counter
    _uuid_counter += 1
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"picowal.{_uuid_counter}"))


# ═══════════════════════════════════════════════════════════════════════
# Symbol generators
# ═══════════════════════════════════════════════════════════════════════

def sym_rp2354b():
    """RP2354B with full GPIO, QSPI, power, USB, SWD, crystal pins."""
    pins = []
    pin_num = 1
    # GPIO0-47
    for i in range(48):
        pins.append(f'(pin bidirectional line (at -20.32 {58.42-i*2.54} 0) (length 2.54)'
                     f' (name "GPIO{i}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Power/system (pins 49-65)
    sys_pins = [
        ("DVDD", "power_in"), ("VREG_VIN", "power_in"), ("VREG_VOUT", "power_out"),
        ("USB_DP", "bidirectional"), ("USB_DM", "bidirectional"),
        ("XIN", "input"), ("XOUT", "output"), ("TESTEN", "input"),
        ("SWCLK", "input"), ("SWDIO", "bidirectional"), ("RUN", "input"),
        ("ADC_AVDD", "power_in"),
        ("GND", "power_in"), ("GND", "power_in"), ("GND", "power_in"),
        ("GND", "power_in"), ("GND", "power_in"),
    ]
    for name, ptype in sys_pins:
        pins.append(f'(pin {ptype} line (at 20.32 {20.32-(pin_num-49)*2.54} 180) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # QSPI (pins 66-71)
    for name in ["QSPI_SCK", "QSPI_CS0", "QSPI_CS1", "QSPI_D0", "QSPI_D1", "QSPI_D2", "QSPI_D3"]:
        pins.append(f'(pin bidirectional line (at 20.32 {-25.4-(pin_num-66)*2.54} 180) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    nl = "\n    "
    return f"""(symbol "rp2354b" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 63.5 0) (effects (font (size 1.27 1.27))))
    (property "Value" "RP2354B" (at 0 -55 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm" (at 0 0 0) (effects (hide yes)))
    (symbol "rp2354b_0_1"
      (rectangle (start -17.78 60.96) (end 17.78 -53) (stroke (width 0.254)) (fill (type background))))
    (symbol "rp2354b_1_1"
    {nl.join(pins)}))"""


def sym_ice40hx8k():
    """iCE40HX8K-CT256 BGA with functional pin groups."""
    pins = []
    pin_num = 1
    groups = [
        # SPI slave from upstream fabric
        ("UP_SPI_SCK", "input"), ("UP_SPI_MOSI", "input"),
        ("UP_SPI_MISO", "output"), ("UP_SPI_CS", "input"),
        # SPI master to RP2354B
        ("RP_SPI_SCK", "output"), ("RP_SPI_MOSI", "output"),
        ("RP_SPI_MISO", "input"), ("RP_SPI_CS", "output"),
        # IRQ + control
        ("RP_IRQ", "output"), ("RP_BUSY", "input"),
        # SPI downstream (to index card or data nodes)
        ("DN_SPI_SCK", "output"), ("DN_SPI_MOSI", "output"),
        ("DN_SPI_MISO", "input"), ("DN_SPI_CS", "output"),
        # SRAM bank A: address + data + control
    ]
    # SRAM bank A address A[17:0]
    for b in range(18):
        groups.append((f"SA_A{b}", "output"))
    # SRAM bank A data D[15:0]
    for b in range(16):
        groups.append((f"SA_D{b}", "bidirectional"))
    # SRAM bank A control
    groups += [("SA_CE_N", "output"), ("SA_OE_N", "output"), ("SA_WE_N", "output")]
    # SRAM bank B address A[17:0]
    for b in range(18):
        groups.append((f"SB_A{b}", "output"))
    # SRAM bank B data D[15:0]
    for b in range(16):
        groups.append((f"SB_D{b}", "bidirectional"))
    # SRAM bank B control
    groups += [("SB_CE_N", "output"), ("SB_OE_N", "output"), ("SB_WE_N", "output")]
    # Config flash SPI
    groups += [("CFG_SCK", "output"), ("CFG_MOSI", "output"),
               ("CFG_MISO", "input"), ("CFG_CS", "output")]
    # CDONE/CRESET
    groups += [("CDONE", "bidirectional"), ("CRESET_B", "input")]
    # Power
    groups += [("VCC", "power_in"), ("VCC_IO", "power_in"),
               ("VCC_PLL", "power_in"), ("GND", "power_in")]

    for name, ptype in groups:
        y = 60 - pin_num * 1.1
        side = -25.4 if pin_num <= len(groups) // 2 else 25.4
        angle = 0 if side < 0 else 180
        pins.append(f'(pin {ptype} line (at {side} {y:.2f} {angle}) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1

    nl = "\n    "
    return f"""(symbol "ice40hx8k_ct256" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 65 0) (effects (font (size 1.27 1.27))))
    (property "Value" "iCE40HX8K-CT256" (at 0 -80 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_BGA:BGA-256_14x14mm_P0.8mm" (at 0 0 0) (effects (hide yes)))
    (symbol "ice40hx8k_ct256_0_1"
      (rectangle (start -22.86 63) (end 22.86 -78) (stroke (width 0.254)) (fill (type background))))
    (symbol "ice40hx8k_ct256_1_1"
    {nl.join(pins)}))"""


def sym_sram():
    """IS61WV25616BLL — 256K×16 async SRAM, TSOP-44."""
    pins = []
    pin_num = 1
    for b in range(18):
        pins.append(f'(pin input line (at -12.7 {20.32-b*2.54} 0) (length 2.54)'
                     f' (name "A{b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    for b in range(16):
        pins.append(f'(pin bidirectional line (at 12.7 {20.32-b*2.54} 180) (length 2.54)'
                     f' (name "D{b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    for name in ["CE_N", "OE_N", "WE_N"]:
        pins.append(f'(pin input line (at -12.7 {-26-(pin_num-35)*2.54} 0) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    for name in ["VCC", "GND"]:
        pins.append(f'(pin power_in line (at 0 {-35-(pin_num-38)*2.54} 90) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    nl = "\n    "
    return f"""(symbol "is61wv25616bll" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 22.86 0) (effects (font (size 1.27 1.27))))
    (property "Value" "IS61WV25616BLL" (at 0 -40 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm" (at 0 0 0) (effects (hide yes)))
    (symbol "is61wv25616bll_0_1"
      (rectangle (start -10.16 21.59) (end 10.16 -38.1) (stroke (width 0.254)) (fill (type background))))
    (symbol "is61wv25616bll_1_1"
    {nl.join(pins)}))"""


def sym_psram():
    """LY68L6400 — 8MB QSPI PSRAM, SOP-8."""
    pins = []
    pin_defs = [
        ("CE#", "input"), ("SO/SIO1", "bidirectional"), ("SIO2", "bidirectional"),
        ("VSS", "power_in"), ("SI/SIO0", "bidirectional"), ("SCLK", "input"),
        ("SIO3", "bidirectional"), ("VCC", "power_in"),
    ]
    for i, (name, ptype) in enumerate(pin_defs):
        side = -10.16 if i < 4 else 10.16
        y = 3.81 - (i % 4) * 2.54
        angle = 0 if side < 0 else 180
        pins.append(f'(pin {ptype} line (at {side} {y} {angle}) (length 2.54)'
                     f' (name "{name}") (number "{i+1}") (uuid {make_uuid()}))')
    nl = "\n    "
    return f"""(symbol "ly68l6400" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
    (property "Value" "LY68L6400_8MB" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (hide yes)))
    (symbol "ly68l6400_0_1"
      (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254)) (fill (type background))))
    (symbol "ly68l6400_1_1"
    {nl.join(pins)}))"""


def sym_generic(ref_prefix, value, fp, pin_names):
    """Generic component symbol."""
    pins = []
    for i, name in enumerate(pin_names):
        ptype = "power_in" if name in ("VCC", "GND", "VIN", "VOUT", "VSS") else "bidirectional"
        pins.append(f'(pin {ptype} line (at -10.16 {5.08-i*2.54} 0) (length 2.54)'
                     f' (name "{name}") (number "{i+1}") (uuid {make_uuid()}))')
    nl = "\n    "
    h = max(len(pin_names) * 2.54 + 2, 8)
    sym_id = value.lower().replace("-", "_").replace(".", "_")
    return f"""(symbol "{sym_id}" (in_bom yes) (on_board yes)
    (property "Reference" "{ref_prefix}" (at 0 {h/2+2} 0) (effects (font (size 1.27 1.27))))
    (property "Value" "{value}" (at 0 {-h/2-2} 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "{fp}" (at 0 0 0) (effects (hide yes)))
    (symbol "{sym_id}_0_1"
      (rectangle (start -7.62 {h/2}) (end 7.62 {-h/2}) (stroke (width 0.254)) (fill (type background))))
    (symbol "{sym_id}_1_1"
    {nl.join(pins)}))"""


# ═══════════════════════════════════════════════════════════════════════
# BOM definitions
# ═══════════════════════════════════════════════════════════════════════

QUERY_PLANNER_BOM = [
    # ── Query planner subsystem ──
    {"ref": "U1",  "value": "iCE40HX8K-CT256",  "pkg": "BGA-256",   "cost": 5.80, "desc": "Bypass/query bridge FPGA"},
    {"ref": "U2",  "value": "RP2354B",           "pkg": "QFN-80",    "cost": 0.70, "desc": "Query planner MCU"},
    {"ref": "U3",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank A (FPGA-owned)"},
    {"ref": "U4",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank B (FPGA-owned)"},
    {"ref": "U5",  "value": "LY68L6400_8MB",     "pkg": "SOP-8",     "cost": 0.85, "desc": "Query RP PSRAM"},
    {"ref": "U6",  "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "Query RP flash"},
    {"ref": "U7",  "value": "W25Q32JVSIQ",       "pkg": "SOIC-8",    "cost": 0.80, "desc": "FPGA config flash"},
    {"ref": "U8",  "value": "TPS62A01",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "Query DVDD buck (OC: 1.1-1.25V)"},
    {"ref": "U9",  "value": "AP2112K-3.3",       "pkg": "SOT-23-5",  "cost": 0.30, "desc": "3.3V IOVDD LDO"},
    {"ref": "U10", "value": "AP2112K-1.2",       "pkg": "SOT-23-5",  "cost": 0.30, "desc": "1.2V FPGA core LDO"},
    # ── Index pico subsystem ──
    {"ref": "U11", "value": "RP2354B",           "pkg": "QFN-80",    "cost": 0.70, "desc": "Index pico MCU"},
    {"ref": "U12", "value": "LY68L6400_8MB",     "pkg": "SOP-8",     "cost": 0.85, "desc": "Index pico PSRAM"},
    {"ref": "U13", "value": "W5500",             "pkg": "LQFP-48",   "cost": 1.80, "desc": "Index pico Ethernet"},
    {"ref": "U14", "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "Index pico flash"},
    {"ref": "U15", "value": "TPS62A01",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "Index DVDD buck (OC: 1.1-1.25V)"},
    # ── Shared ──
    {"ref": "Y1",  "value": "12MHz",             "pkg": "3215",      "cost": 0.20, "desc": "RP crystal (both picos)"},
    {"ref": "Y2",  "value": "25MHz",             "pkg": "3215",      "cost": 0.20, "desc": "W5500 crystal"},
    {"ref": "J1",  "value": "CONN_2x10",         "pkg": "2x10-2.54", "cost": 0.30, "desc": "Upstream SPI + power"},
    {"ref": "J2",  "value": "CONN_2x10",         "pkg": "2x10-2.54", "cost": 0.30, "desc": "Downstream SPI + power"},
    {"ref": "J3",  "value": "CONN_1x4",          "pkg": "1x4-2.54",  "cost": 0.10, "desc": "UART debug"},
    {"ref": "J4",  "value": "CONN_1x5",          "pkg": "1x5-1.27",  "cost": 0.10, "desc": "SWD query pico"},
    {"ref": "J5",  "value": "RJ45_MAGJACK",      "pkg": "RJ45",      "cost": 3.00, "desc": "Index pico Ethernet jack"},
    {"ref": "J6",  "value": "CONN_1x5",          "pkg": "1x5-1.27",  "cost": 0.10, "desc": "SWD index pico"},
]


# ═══════════════════════════════════════════════════════════════════════
# Query Planner schematic
# ═══════════════════════════════════════════════════════════════════════

def gen_query_planner_sch():
    syms = [
        sym_ice40hx8k(),
        sym_rp2354b(),
        sym_sram(),
        sym_psram(),
        sym_generic("U", "W25Q128JVSIQ", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
                    ["SCK", "CS", "DI", "DO", "WP", "HOLD", "VCC", "GND"]),
        sym_generic("U", "W25Q32JVSIQ", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
                    ["SCK", "CS", "DI", "DO", "WP", "HOLD", "VCC", "GND"]),
        sym_generic("U", "W5500", "Package_QFP:LQFP-48_7x7mm_P0.5mm",
                    ["SCK", "MOSI", "MISO", "CS", "INT", "RST",
                     "TXP", "TXN", "RXP", "RXN", "LINKLED", "ACTLED",
                     "VCC", "GND"]),
        sym_generic("U", "TPS62A01", "Package_TO_SOT_SMD:SOT-23-6",
                    ["VIN", "SW", "GND", "EN", "FB", "PG"]),
        sym_generic("U", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5",
                    ["VIN", "GND", "EN", "NC", "VOUT"]),
        sym_generic("U", "AP2112K-1.2", "Package_TO_SOT_SMD:SOT-23-5",
                    ["VIN", "GND", "EN", "NC", "VOUT"]),
        sym_generic("Y", "12MHz", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
                    ["XIN", "XOUT"]),
        sym_generic("Y", "25MHz", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
                    ["XIN", "XOUT"]),
        sym_generic("J", "CONN_2x10", "Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical",
                    [f"P{i+1}" for i in range(20)]),
        sym_generic("J", "CONN_1x4", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
                    ["TX", "RX", "VCC", "GND"]),
        sym_generic("J", "CONN_1x5", "Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical",
                    ["SWCLK", "SWDIO", "RUN", "VCC", "GND"]),
        sym_generic("J", "RJ45_MAGJACK", "Connector_RJ45:RJ45_Amphenol_ARJM11C7",
                    ["TD+", "TD-", "RD+", "RD-", "LED1", "LED2", "VCC", "GND"]),
    ]

    instances = []
    x, y = 50, 80

    # FPGA — center stage
    instances.append(f"""(symbol (lib_id "ice40hx8k_ct256") (at {x+90} {y} 0) (unit 1)
    (property "Reference" "U1" (at 0 2 0)) (property "Value" "iCE40HX8K-CT256" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U1") (unit 1)))))""")

    # RP2354B — left
    instances.append(f"""(symbol (lib_id "rp2354b") (at {x} {y} 0) (unit 1)
    (property "Reference" "U2" (at 0 2 0)) (property "Value" "RP2354B" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U2") (unit 1)))))""")

    # 2× SRAM banks
    for i, ref in enumerate(["U3", "U4"]):
        sx = x + 180 + i * 45
        instances.append(f"""(symbol (lib_id "is61wv25616bll") (at {sx} {y-30} 0) (unit 1)
    (property "Reference" "{ref}" (at 0 2 0)) (property "Value" "IS61WV25616BLL" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "{ref}") (unit 1)))))""")

    # PSRAM
    instances.append(f"""(symbol (lib_id "ly68l6400") (at {x+40} {y+50} 0) (unit 1)
    (property "Reference" "U5" (at 0 2 0)) (property "Value" "LY68L6400_8MB" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U5") (unit 1)))))""")

    # Flash chips
    instances.append(f"""(symbol (lib_id "w25q128jvsiq") (at {x} {y+50} 0) (unit 1)
    (property "Reference" "U6" (at 0 2 0)) (property "Value" "W25Q128JVSIQ" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U6") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "w25q32jvsiq") (at {x+90} {y+80} 0) (unit 1)
    (property "Reference" "U7" (at 0 2 0)) (property "Value" "W25Q32JVSIQ" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U7") (unit 1)))))""")

    # Power regulators
    instances.append(f"""(symbol (lib_id "tps62a01") (at {x-40} {y-20} 0) (unit 1)
    (property "Reference" "U8" (at 0 2 0)) (property "Value" "TPS62A01" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U8") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "ap2112k_3_3") (at {x-40} {y} 0) (unit 1)
    (property "Reference" "U9" (at 0 2 0)) (property "Value" "AP2112K-3.3" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U9") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "ap2112k_1_2") (at {x-40} {y+20} 0) (unit 1)
    (property "Reference" "U10" (at 0 2 0)) (property "Value" "AP2112K-1.2" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U10") (unit 1)))))""")

    # Crystal
    instances.append(f"""(symbol (lib_id "12mhz") (at {x+30} {y+30} 0) (unit 1)
    (property "Reference" "Y1" (at 0 2 0)) (property "Value" "12MHz" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "Y1") (unit 1)))))""")

    # Connectors
    instances.append(f"""(symbol (lib_id "conn_2x10") (at {x-60} {y} 0) (unit 1)
    (property "Reference" "J1" (at 0 2 0)) (property "Value" "Upstream" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J1") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "conn_2x10") (at {x+280} {y} 0) (unit 1)
    (property "Reference" "J2" (at 0 2 0)) (property "Value" "Downstream" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J2") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "conn_1x4") (at {x+280} {y+40} 0) (unit 1)
    (property "Reference" "J3" (at 0 2 0)) (property "Value" "UART_DBG" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J3") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "conn_1x5") (at {x+280} {y+60} 0) (unit 1)
    (property "Reference" "J4" (at 0 2 0)) (property "Value" "SWD_QRY" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J4") (unit 1)))))""")

    # ── Index Pico subsystem (separate RP2354B + W5500 + RJ45) ──

    # Index RP2354B — right side, dedicated to index block I/O
    instances.append(f"""(symbol (lib_id "rp2354b") (at {x+180} {y+60} 0) (unit 1)
    (property "Reference" "U11" (at 0 2 0)) (property "Value" "RP2354B_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U11") (unit 1)))))""")

    # Index pico PSRAM
    instances.append(f"""(symbol (lib_id "ly68l6400") (at {x+220} {y+60} 0) (unit 1)
    (property "Reference" "U12" (at 0 2 0)) (property "Value" "LY68L6400_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U12") (unit 1)))))""")

    # W5500 Ethernet controller for index pico
    instances.append(f"""(symbol (lib_id "w5500") (at {x+180} {y+100} 0) (unit 1)
    (property "Reference" "U13" (at 0 2 0)) (property "Value" "W5500" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U13") (unit 1)))))""")

    # 25MHz crystal for W5500
    instances.append(f"""(symbol (lib_id "25mhz") (at {x+210} {y+100} 0) (unit 1)
    (property "Reference" "Y2" (at 0 2 0)) (property "Value" "25MHz" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "Y2") (unit 1)))))""")

    # RJ45 MagJack for index pico Ethernet
    instances.append(f"""(symbol (lib_id "rj45_magjack") (at {x+250} {y+100} 0) (unit 1)
    (property "Reference" "J5" (at 0 2 0)) (property "Value" "RJ45" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J5") (unit 1)))))""")

    # Index pico flash
    instances.append(f"""(symbol (lib_id "w25q128jvsiq") (at {x+220} {y+80} 0) (unit 1)
    (property "Reference" "U14" (at 0 2 0)) (property "Value" "W25Q128_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U14") (unit 1)))))""")

    # OC buck for index pico
    instances.append(f"""(symbol (lib_id "tps62a01") (at {x+150} {y+70} 0) (unit 1)
    (property "Reference" "U15" (at 0 2 0)) (property "Value" "TPS62A01_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U15") (unit 1)))))""")

    # SWD for index pico
    instances.append(f"""(symbol (lib_id "conn_1x5") (at {x+280} {y+80} 0) (unit 1)
    (property "Reference" "J6" (at 0 2 0)) (property "Value" "SWD_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J6") (unit 1)))))""")

    # Net labels — FPGA↔SRAM, FPGA↔RPs, upstream/downstream SPI, index pico Ethernet
    netlabels = []
    bus_nets = [
        # Upstream SPI
        "UP_SPI_SCK", "UP_SPI_MOSI", "UP_SPI_MISO", "UP_SPI_CS",
        # FPGA↔Query RP SPI
        "RP_SPI_SCK", "RP_SPI_MOSI", "RP_SPI_MISO", "RP_SPI_CS",
        "RP_IRQ", "RP_BUSY",
        # Downstream SPI (data path bypass)
        "DN_SPI_SCK", "DN_SPI_MOSI", "DN_SPI_MISO", "DN_SPI_CS",
        # FPGA↔Index RP SPI (index path)
        "IDX_SPI_SCK", "IDX_SPI_MOSI", "IDX_SPI_MISO", "IDX_SPI_CS",
        "IDX_IRQ",
        # Index RP↔W5500 SPI
        "ETH_SPI_SCK", "ETH_SPI_MOSI", "ETH_SPI_MISO", "ETH_SPI_CS",
        "ETH_INT", "ETH_RST",
        # Power rails
        "VCC_3V3", "VCC_1V2", "DVDD_QRY", "DVDD_IDX", "GND",
    ]
    # SRAM bank A/B address + data
    for bank in ["SA", "SB"]:
        for b in range(18):
            bus_nets.append(f"{bank}_A{b}")
        for b in range(16):
            bus_nets.append(f"{bank}_D{b}")
        bus_nets += [f"{bank}_CE_N", f"{bank}_OE_N", f"{bank}_WE_N"]

    for net in bus_nets:
        shape = "bidirectional" if "SPI" in net or "_D" in net else "output"
        if net in ("VCC_3V3", "VCC_1V2", "DVDD", "GND"):
            shape = "passive"
        netlabels.append(f"""(global_label "{net}" (at 0 0 0) (shape {shape})
    (uuid {make_uuid()}) (property "Intersheetrefs" "" (at 0 0 0)))""")

    nl = "\n  "
    return f"""(kicad_sch (version 20231120) (generator "picowal_gen") (generator_version "{VERSION}")
  (paper "A2")
  (title_block
    (title "PicoWAL Query Planner v{VERSION}")
    (comment 1 "iCE40HX8K owns all buses. 2× RP2354B are pure control plane via FPGA SPI register map.")
    (comment 2 "FAST PATH: addr[52]=0 → FPGA bypass → downstream SATA nodes (zero copy, zero pico)")
    (comment 3 "QUERY PATH: addr[52]=1 → FPGA queues in SRAM FIFO → query pico issues copy cmds via FPGA")
    (comment 4 "INDEX: data writes → FPGA IRQs index pico → pico reads/writes index blocks via FPGA cmds"))
  (lib_symbols
  {nl.join(syms)})
  {nl.join(instances)}
  {nl.join(netlabels)})"""


# ═══════════════════════════════════════════════════════════════════════
# PCB generation
# ═══════════════════════════════════════════════════════════════════════

def make_fp(ref, footprint, value, x, y):
    return f"""(footprint "{footprint}"
    (at {x} {y}) (layer "F.Cu") (uuid {make_uuid()})
    (property "Reference" "{ref}" (at 0 -2 0) (layer "F.SilkS") (uuid {make_uuid()})
      (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "{value}" (at 0 2 0) (layer "F.SilkS") (uuid {make_uuid()})
      (effects (font (size 0.8 0.8) (thickness 0.12)))))"""


def gen_pcb(board_name, bom, board_w=70, board_h=50):
    footprints = []
    # Layout: regulators left, MCU center-left, FPGA center, memory right
    layout = {
        "query_planner": {
            # Query planner subsystem (left half)
            "U1": ("Package_BGA:BGA-256_14x14mm_P0.8mm", "iCE40HX8K", 30, 25),
            "U2": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_QRY", 12, 25),
            "U3": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_A", 48, 10),
            "U4": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_B", 48, 40),
            "U5": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_QRY", 12, 42),
            "U6": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_QRY", 5, 10),
            "U7": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q32", 30, 42),
            "U8": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_QRY", 3, 20),
            "U9": ("Package_TO_SOT_SMD:SOT-23-5", "AP2112K_3V3", 3, 28),
            "U10": ("Package_TO_SOT_SMD:SOT-23-5", "AP2112K_1V2", 3, 34),
            # Index pico subsystem (right half)
            "U11": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_IDX", 68, 25),
            "U12": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_IDX", 68, 42),
            "U13": ("Package_QFP:LQFP-48_7x7mm_P0.5mm", "W5500", 82, 25),
            "U14": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_IDX", 68, 10),
            "U15": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_IDX", 60, 20),
            # Shared
            "Y1": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "12MHz", 18, 15),
            "Y2": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "25MHz", 82, 15),
            "J1": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "UP", 0, 25),
            "J2": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "DN", 55, 50),
            "J3": ("Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", "UART", 55, 5),
            "J4": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_QRY", 40, 50),
            "J5": ("Connector_RJ45:RJ45_Amphenol_ARJM11C7", "RJ45", 92, 25),
            "J6": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_IDX", 78, 50),
        },
    }

    for ref, (fp, val, px, py) in layout[board_name].items():
        footprints.append(make_fp(ref, fp, val, px, py))

    edge = f"""(gr_rect (start 0 0) (end {board_w} {board_h})
    (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts") (uuid {make_uuid()}))"""

    nl = "\n  "
    return f"""(kicad_pcb (version 20231014) (generator "picowal_gen") (generator_version "{VERSION}")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers
    (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal)
    (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")
    (44 "Edge.Cuts" user))
  (setup (grid_origin 0 0)
    (pcbplotparams (layerselection 0x00010fc_ffffffff) (outputdirectory "")))
  {edge}
  {nl.join(footprints)})"""


# ═══════════════════════════════════════════════════════════════════════
# Project file
# ═══════════════════════════════════════════════════════════════════════

def gen_project():
    return """{
  "meta": {"filename": "picowal.kicad_pro", "version": 2},
  "board": {"design_settings": {"defaults": {"board_outline_line_width": 0.1},
    "rules": {"min_clearance": 0.15, "min_track_width": 0.15}}},
  "schematic": {"drawing": {"default_line_thickness": 0.006}},
  "sheets": [
    ["query_planner.kicad_sch", "PicoWAL Query Processor"]
  ]
}"""


# ═══════════════════════════════════════════════════════════════════════
# Pin budget & power budget reports
# ═══════════════════════════════════════════════════════════════════════

def pin_budget_report():
    print("\n  Pin Budget — iCE40HX8K-CT256 (208 I/O max)")
    print("  " + "─" * 55)
    pins = [
        ("Upstream SPI (slave)", 4),
        ("Downstream SPI (master, bypass+copy)", 4),
        ("Query pico SPI (slave, register map)", 4),
        ("Index pico SPI (slave, register map)", 4),
        ("Query pico IRQ", 1),
        ("Index pico IRQ", 1),
        ("SRAM bank A (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("SRAM bank B (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("Config flash SPI", 4),
        ("CDONE/CRESET_B", 2),
        ("Status LEDs", 4),
    ]
    total = 0
    for name, n in pins:
        print(f"    {name:48s} {n:3d}")
        total += n
    print(f"    {'─'*48} {'─'*3}")
    print(f"    {'TOTAL':48s} {total:3d} / 208  ({'OK' if total <= 208 else 'OVER'})")


def power_budget_report():
    print("\n  Power Budget — Single card, 3.3V input")
    print("  " + "─" * 55)
    rails = [
        ("iCE40HX8K core (1.2V, 80mA)", 0.096),
        ("iCE40HX8K I/O (3.3V, 40mA)", 0.132),
        ("RP2354B #1 QRY DVDD (1.1V, 100mA)", 0.110),
        ("RP2354B #1 QRY IOVDD (3.3V, 50mA)", 0.165),
        ("RP2354B #2 IDX DVDD (1.1V, 100mA)", 0.110),
        ("RP2354B #2 IDX IOVDD (3.3V, 50mA)", 0.165),
        ("2× IS61WV25616BLL (3.3V, 40mA each)", 0.264),
        ("2× LY68L6400 PSRAM (3.3V, 25mA each)", 0.165),
        ("W5500 Ethernet (3.3V, 130mA)", 0.429),
        ("Flash chips (3.3V, 40mA total)", 0.132),
        ("OC headroom (+25% on both DVDD)", 0.055),
    ]
    total = 0
    for name, w in rails:
        print(f"    {name:48s} {w:.3f}W")
        total += w
    print(f"    {'─'*48} {'─'*5}")
    print(f"    {'TOTAL':48s} {total:.3f}W")


# ═══════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Project file
    with open(os.path.join(OUTPUT_DIR, "picowal.kicad_pro"), "w") as f:
        f.write(gen_project())
    print("[OK] picowal.kicad_pro")

    # Query Planner (single card with both picos)
    with open(os.path.join(OUTPUT_DIR, "query_planner.kicad_sch"), "w") as f:
        f.write(gen_query_planner_sch())
    print("[OK] query_planner.kicad_sch")

    # PCB
    with open(os.path.join(OUTPUT_DIR, "query_planner.kicad_pcb"), "w") as f:
        f.write(gen_pcb("query_planner", QUERY_PLANNER_BOM, board_w=100, board_h=55))
    print("[OK] query_planner.kicad_pcb")

    # Summary
    print(f"\n{'='*70}")
    print(f" PicoWAL v{VERSION} — Single Query Processor Card")
    print(f"{'='*70}")
    print()
    print(" Architecture: FPGA owns everything. Picos are pure control plane.")
    print()
    print("   iCE40HX8K-CT256    SPI router + SRAM FIFO + block copy engine")
    print("   RP2354B #1 (QRY)   Drains query FIFO, issues copy cmds to FPGA")
    print("   RP2354B #2 (IDX)   Updates index blocks on SATA via FPGA cmds")
    print("   2× IS61WV25616BLL  FPGA-owned SRAM (query FIFO + page staging)")
    print("   2× LY68L6400       PSRAM per pico (working set only)")
    print("   W5500 + RJ45       Index pico Ethernet (reads/writes index blocks)")
    print("   Board:              100×55mm, 4-layer")
    print()
    print(" Address format: [63:53]=tenant [52]=INDEX [51:42]=card [41:0]=block")
    print("   addr[52]=0  DATA   → FPGA bypass → downstream SATA nodes")
    print("   addr[52]=1  INDEX  → FPGA queues → query pico processes")
    print()
    print(" Pico command interface (SPI register map via FPGA):")
    print("   CMD_FIFO_POP   0x10  Query pico reads next descriptor from FIFO")
    print("   CMD_IDX_READ   0x01  Read index block from SATA via FPGA")
    print("   CMD_IDX_WRITE  0x05  Write index block to SATA via FPGA")
    print("   CMD_COPY_OUT   0x02  FPGA reads data block → streams to upstream")
    print("   CMD_MULTI_START 0x03 Begin multi-block gather")
    print("   CMD_MULTI_END  0x04  Flush gathered response")
    print("   CMD_NOTIFY_ACK 0x11  Index pico acks write notification")
    print()

    print(" BOM:")
    total = sum(p["cost"] for p in QUERY_PLANNER_BOM) + 5.0 + 2.50
    for part in QUERY_PLANNER_BOM:
        print(f"   {part['ref']:6s}  {part['value']:20s}  £{part['cost']:.2f}  {part['desc']}")
    print(f"   {'PCB':6s}  {'4-layer 100×55mm':20s}  £5.00")
    print(f"   {'MISC':6s}  {'Passives+bypass':20s}  £2.50")
    print(f"   {'─'*57}")
    print(f"   {'TOTAL':6s}  {'':20s}  £{total:.2f}")
    print()

    print(" OC Configuration (both picos):")
    print("   TPS62A01 DVDD: R_FB1=100kΩ, R_FB2=91kΩ → 1.1V (stock)")
    print("   OC mode:       R_FB2=82kΩ → 1.15V | R_FB2=68kΩ → 1.25V")
    print("   RP2354B PLL:   stock 150MHz | OC target 200MHz")
    print("   Extra decoupling: 10μF + 100nF per DVDD pin")
    print()

    print(" Data flow:")
    print("   ┌─ FAST PATH (addr[52]=0) ─────────────────────────┐")
    print("   │ upstream SPI → FPGA bypass → downstream SATA KV  │")
    print("   │ zero copy, zero pico, ~2 clk overhead            │")
    print("   └──────────────────────────────────────────────────┘")
    print("   ┌─ QUERY PATH (addr[52]=1) ────────────────────────┐")
    print("   │ upstream → FPGA SRAM FIFO → qry_irq              │")
    print("   │ query pico: FIFO_POP → IDX_READ → COPY_OUT       │")
    print("   │ FPGA does all data movement, pico just commands   │")
    print("   └──────────────────────────────────────────────────┘")
    print("   ┌─ INDEX UPDATE (on data writes) ──────────────────┐")
    print("   │ FPGA bypass + idx_irq                             │")
    print("   │ index pico: IDX_READ → update → IDX_WRITE         │")
    print("   │ indexes stored as blocks with addr[52]=1 on SATA  │")
    print("   └──────────────────────────────────────────────────┘")

    pin_budget_report()
    power_budget_report()

    print(f"\n{'='*70}")


if __name__ == "__main__":
    main()
