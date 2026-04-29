#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""generate_picowal.py -- PicoWAL Query Processor Card

Generates KiCad 8 schematic + PCB for a single PicoWAL query processor:

  - 1x ECP5-5G-45F (LFE5UM5G-45F-CABGA381): FPGA with 4x 3.125Gbps SerDes
  - 3x M.2 M-key NVMe slots: PCIe Gen1 x1 each via ECP5 SerDes (RAID)
  - 2x RP2354B: query pico + index pico (deterministic stream processors)
  - 2x IS61WV25616BLL: FPGA-owned page staging (512KB, parallel banks)
  - 2x LY68L6400: RP-owned 8MB PSRAM (one per pico)
  - 1x RTL8221B + RJ45: 2.5GbE PHY via SerDes CH3, PoE powered
  - OC: TPS62A01 adjustable buck for DVDD (1.1-1.25V)

Deterministic architecture:
  - Each pico owns dedicated TCP streams via 8-bit parallel bus (15 pins)
  - FPGA handles MAC/IP/TCP in hardware (TOE), presents byte streams to picos
  - Direct block reads bypass picos entirely (FPGA -> NVMe -> TCP)
  - All writes serialized through index pico (no contention)
  - Drive 0,1 = data shards (read-optimized)
  - Drive 2 = WAL + indexes (index pico exclusive)

8-bit parallel bus protocol:
  DATA[7:0] + RDY + ACK + DIR + SOF + EOF + SOCK[1:0]
  Clocked handshake: sender asserts RDY, receiver pulses ACK.
  One dead cycle on DIR change. Timeout recovery on stall.
  PIO+DMA on pico side for zero-CPU data movement.

Address format: [63:53]=tenant [52]=INDEX [51:42]=card [41:0]=block
  addr[52]=0: DATA  -> FPGA bypass -> NVMe drives (zero copy)
  addr[52]=1: INDEX -> FPGA queues in SRAM FIFO -> pico processes

Board: ~130x80mm, 4-layer, BGA/TSOP/M.2 -- requires reflow.
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


def sym_ecp5():
    """ECP5-5G-45F (LFE5UM5G-45F) CABGA381 with SerDes/PCIe + functional pin groups."""
    pins = []
    pin_num = 1
    groups = [
        # SPI slave from upstream fabric
        ("UP_SPI_SCK", "input"), ("UP_SPI_MOSI", "input"),
        ("UP_SPI_MISO", "output"), ("UP_SPI_CS", "input"),
        # 8-bit parallel bus to query RP2354B (15 pins)
        ("QRY_D0", "bidirectional"), ("QRY_D1", "bidirectional"),
        ("QRY_D2", "bidirectional"), ("QRY_D3", "bidirectional"),
        ("QRY_D4", "bidirectional"), ("QRY_D5", "bidirectional"),
        ("QRY_D6", "bidirectional"), ("QRY_D7", "bidirectional"),
        ("QRY_RDY", "output"), ("QRY_ACK", "input"),
        ("QRY_DIR", "output"), ("QRY_SOF", "output"),
        ("QRY_EOF", "output"),
        ("QRY_SOCK0", "output"), ("QRY_SOCK1", "output"),
        # 8-bit parallel bus to index RP2354B (15 pins)
        ("IDX_D0", "bidirectional"), ("IDX_D1", "bidirectional"),
        ("IDX_D2", "bidirectional"), ("IDX_D3", "bidirectional"),
        ("IDX_D4", "bidirectional"), ("IDX_D5", "bidirectional"),
        ("IDX_D6", "bidirectional"), ("IDX_D7", "bidirectional"),
        ("IDX_RDY", "output"), ("IDX_ACK", "input"),
        ("IDX_DIR", "output"), ("IDX_SOF", "output"),
        ("IDX_EOF", "output"),
        ("IDX_SOCK0", "output"), ("IDX_SOCK1", "output"),
        # Downstream SPI (legacy / expansion)
        ("DN_SPI_SCK", "output"), ("DN_SPI_MOSI", "output"),
        ("DN_SPI_MISO", "input"), ("DN_SPI_CS", "output"),
    ]
    # SerDes / PCIe channels (3× NVMe + 1 spare)
    for ch in range(4):
        groups += [
            (f"HDOUTP{ch}", "output"), (f"HDOUTN{ch}", "output"),
            (f"HDINP{ch}", "input"), (f"HDINTN{ch}", "input"),
        ]
    # PCIe reference clock (differential)
    groups += [("REFCLKP", "input"), ("REFCLKN", "input")]
    # PCIe per-slot control
    for slot in range(3):
        groups += [
            (f"PERST{slot}_N", "output"),
            (f"CLKREQ{slot}_N", "input"),
        ]
    # SRAM bank A: address + data + control
    for b in range(18):
        groups.append((f"SA_A{b}", "output"))
    for b in range(16):
        groups.append((f"SA_D{b}", "bidirectional"))
    groups += [("SA_CE_N", "output"), ("SA_OE_N", "output"), ("SA_WE_N", "output")]
    # SRAM bank B: address + data + control
    for b in range(18):
        groups.append((f"SB_A{b}", "output"))
    for b in range(16):
        groups.append((f"SB_D{b}", "bidirectional"))
    groups += [("SB_CE_N", "output"), ("SB_OE_N", "output"), ("SB_WE_N", "output")]
    # Config flash SPI (ECP5 uses same MSPI boot)
    groups += [("CFG_SCK", "output"), ("CFG_MOSI", "output"),
               ("CFG_MISO", "input"), ("CFG_CS", "output")]
    # DONE/INITN/PROGRAMN
    groups += [("DONE", "bidirectional"), ("INITN", "bidirectional"),
               ("PROGRAMN", "input")]
    # Power
    groups += [("VCC", "power_in"), ("VCCIO", "power_in"),
               ("VCCAUX", "power_in"), ("VCCHTX", "power_in"),
               ("VCCHRX", "power_in"), ("GND", "power_in")]

    for name, ptype in groups:
        y = 80 - pin_num * 1.0
        side = -27.94 if pin_num <= len(groups) // 2 else 27.94
        angle = 0 if side < 0 else 180
        pins.append(f'(pin {ptype} line (at {side} {y:.2f} {angle}) (length 2.54)'
                     f' (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1

    nl = "\n    "
    return f"""(symbol "ecp5_5g_45f" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 85 0) (effects (font (size 1.27 1.27))))
    (property "Value" "LFE5UM5G-45F-CABGA381" (at 0 -95 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_BGA:BGA-381_17x17mm_P0.8mm" (at 0 0 0) (effects (hide yes)))
    (symbol "ecp5_5g_45f_0_1"
      (rectangle (start -25.4 83) (end 25.4 -93) (stroke (width 0.254)) (fill (type background))))
    (symbol "ecp5_5g_45f_1_1"
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


def sym_m2_mkey():
    """M.2 M-key connector — simplified to functional NVMe/PCIe pins."""
    pins = []
    pin_defs = [
        ("PERST_N", "input"), ("CLKREQ_N", "output"), ("PEWAKE_N", "output"),
        ("PCIE_TX_P", "input"), ("PCIE_TX_N", "input"),
        ("PCIE_RX_P", "output"), ("PCIE_RX_N", "output"),
        ("REFCLK_P", "input"), ("REFCLK_N", "input"),
        ("VCC_3V3", "power_in"), ("GND", "power_in"),
    ]
    for i, (name, ptype) in enumerate(pin_defs):
        side = -12.7 if i < 6 else 12.7
        y = 6.35 - (i % 6) * 2.54
        angle = 0 if side < 0 else 180
        pins.append(f'(pin {ptype} line (at {side} {y} {angle}) (length 2.54)'
                     f' (name "{name}") (number "{i+1}") (uuid {make_uuid()}))')
    nl = "\n    "
    return f"""(symbol "m2_mkey_nvme" (in_bom yes) (on_board yes)
    (property "Reference" "J" (at 0 10.16 0) (effects (font (size 1.27 1.27))))
    (property "Value" "M.2_M-Key_NVMe" (at 0 -10.16 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Connector_M2:M2_M_Key_22x80" (at 0 0 0) (effects (hide yes)))
    (symbol "m2_mkey_nvme_0_1"
      (rectangle (start -10.16 8.89) (end 10.16 -8.89) (stroke (width 0.254)) (fill (type background))))
    (symbol "m2_mkey_nvme_1_1"
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
    # ── FPGA + NVMe subsystem ──
    {"ref": "U1",  "value": "LFE5UM5G-45F",     "pkg": "CABGA-381", "cost": 10.00, "desc": "ECP5-5G FPGA, 4× SerDes, PCIe Gen1 + SGMII"},
    {"ref": "U2",  "value": "RP2354B",           "pkg": "QFN-80",    "cost": 0.70, "desc": "Query planner MCU"},
    {"ref": "U3",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank A (FPGA-owned)"},
    {"ref": "U4",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank B (FPGA-owned)"},
    {"ref": "U5",  "value": "LY68L6400_8MB",     "pkg": "SOP-8",     "cost": 0.85, "desc": "Query RP PSRAM"},
    {"ref": "U6",  "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "Query RP flash"},
    {"ref": "U7",  "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "FPGA config flash (128Mbit for ECP5-45F)"},
    {"ref": "U8",  "value": "TPS62A01",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "Query DVDD buck (OC: 1.1-1.25V)"},
    {"ref": "U9",  "value": "AP2112K-3.3",       "pkg": "SOT-23-5",  "cost": 0.30, "desc": "3.3V IOVDD LDO"},
    {"ref": "U10", "value": "TPS62A02",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "1.1V ECP5 core buck"},
    # ── NVMe storage ──
    {"ref": "J7",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 0 (PCIe Gen1 x1, SerDes CH0)"},
    {"ref": "J8",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 1 (PCIe Gen1 x1, SerDes CH1)"},
    {"ref": "J9",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 2 (PCIe Gen1 x1, SerDes CH2)"},
    {"ref": "Y3",  "value": "100MHz_DIFF",       "pkg": "3225",      "cost": 1.80, "desc": "PCIe reference clock (LVDS, shared fanout)"},
    {"ref": "U16", "value": "TPS54331",          "pkg": "SOIC-8",    "cost": 1.20, "desc": "3.3V 3A buck for NVMe drives"},
    # ── GbE + PoE (FPGA TOE via SerDes CH3) ──
    {"ref": "U13", "value": "RTL8221B",          "pkg": "QFN-56",    "cost": 1.50, "desc": "2.5GbE PHY, 2500BASE-X to ECP5 SerDes CH3"},
    {"ref": "U17", "value": "TPS23753A",         "pkg": "TSSOP-20",  "cost": 2.50, "desc": "802.3at PoE PD + DC-DC controller (25.5W)"},
    {"ref": "J5",  "value": "RJ45_POE_MAGJACK",  "pkg": "RJ45",      "cost": 4.00, "desc": "RJ45 with PoE center taps + magnetics"},
    # ── Index pico subsystem ──
    {"ref": "U11", "value": "RP2354B",           "pkg": "QFN-80",    "cost": 0.70, "desc": "Index pico MCU"},
    {"ref": "U12", "value": "LY68L6400_8MB",     "pkg": "SOP-8",     "cost": 0.85, "desc": "Index pico PSRAM"},
    {"ref": "U14", "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "Index pico flash"},
    {"ref": "U15", "value": "TPS62A01",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "Index DVDD buck (OC: 1.1-1.25V)"},
    # ── Shared ──
    {"ref": "Y1",  "value": "12MHz",             "pkg": "3215",      "cost": 0.20, "desc": "RP crystal (both picos)"},
    {"ref": "Y2",  "value": "25MHz",             "pkg": "3215",      "cost": 0.20, "desc": "RTL8221B PHY crystal"},
    {"ref": "J1",  "value": "CONN_2x10",         "pkg": "2x10-2.54", "cost": 0.30, "desc": "Upstream SPI + power"},
    {"ref": "J2",  "value": "CONN_2x10",         "pkg": "2x10-2.54", "cost": 0.30, "desc": "Downstream SPI + power"},
    {"ref": "J3",  "value": "CONN_1x4",          "pkg": "1x4-2.54",  "cost": 0.10, "desc": "UART debug"},
    {"ref": "J4",  "value": "CONN_1x5",          "pkg": "1x5-1.27",  "cost": 0.10, "desc": "SWD query pico"},
    {"ref": "J6",  "value": "CONN_1x5",          "pkg": "1x5-1.27",  "cost": 0.10, "desc": "SWD index pico"},
]


# ═══════════════════════════════════════════════════════════════════════
# Query Planner schematic
# ═══════════════════════════════════════════════════════════════════════

def gen_query_planner_sch():
    syms = [
        sym_ecp5(),
        sym_rp2354b(),
        sym_sram(),
        sym_psram(),
        sym_m2_mkey(),
        sym_generic("U", "W25Q128JVSIQ", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
                    ["SCK", "CS", "DI", "DO", "WP", "HOLD", "VCC", "GND"]),
        sym_generic("U", "RTL8221B", "Package_DFN_QFN:QFN-56_7x7mm_P0.4mm",
                    ["SGMII_TXP", "SGMII_TXN", "SGMII_RXP", "SGMII_RXN",
                     "MDC", "MDIO", "PHYRST_N", "INTB",
                     "TXP", "TXN", "RXP", "RXN",
                     "LED0", "LED1", "LED2",
                     "XIN", "XOUT",
                     "AVDD33", "DVDD10", "GND"]),
        sym_generic("U", "TPS23753A", "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm",
                    ["VDD", "VSS", "APD", "DET", "CLS",
                     "GATE", "RTN", "VC", "COMP", "FB",
                     "EN", "PG", "CTL", "FRS"]),
        sym_generic("U", "TPS62A01", "Package_TO_SOT_SMD:SOT-23-6",
                    ["VIN", "SW", "GND", "EN", "FB", "PG"]),
        sym_generic("U", "TPS62A02", "Package_TO_SOT_SMD:SOT-23-6",
                    ["VIN", "SW", "GND", "EN", "FB", "PG"]),
        sym_generic("U", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5",
                    ["VIN", "GND", "EN", "NC", "VOUT"]),
        sym_generic("U", "TPS54331", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
                    ["BOOT", "VIN", "EN", "SS", "VSENSE", "COMP", "PH", "GND"]),
        sym_generic("Y", "12MHz", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
                    ["XIN", "XOUT"]),
        sym_generic("Y", "25MHz", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
                    ["XIN", "XOUT"]),
        sym_generic("Y", "100MHz_DIFF", "Oscillator:Oscillator_SMD_3225",
                    ["OUT+", "OUT-", "EN", "VCC", "GND"]),
        sym_generic("J", "CONN_2x10", "Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical",
                    [f"P{i+1}" for i in range(20)]),
        sym_generic("J", "CONN_1x4", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
                    ["TX", "RX", "VCC", "GND"]),
        sym_generic("J", "CONN_1x5", "Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical",
                    ["SWCLK", "SWDIO", "RUN", "VCC", "GND"]),
        sym_generic("J", "RJ45_POE_MAGJACK", "Connector_RJ45:RJ45_Amphenol_ARJM11C7",
                    ["TD+", "TD-", "RD+", "RD-", "CT_P", "CT_N",
                     "LED1", "LED2", "VCC", "GND"]),
    ]

    instances = []
    x, y = 50, 80

    # ECP5 FPGA — center stage
    instances.append(f"""(symbol (lib_id "ecp5_5g_45f") (at {x+90} {y} 0) (unit 1)
    (property "Reference" "U1" (at 0 2 0)) (property "Value" "LFE5UM5G-45F" (at 0 -2 0))
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

    instances.append(f"""(symbol (lib_id "w25q128jvsiq") (at {x+90} {y+80} 0) (unit 1)
    (property "Reference" "U7" (at 0 2 0)) (property "Value" "W25Q128_CFG" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U7") (unit 1)))))""")

    # Power regulators
    instances.append(f"""(symbol (lib_id "tps62a01") (at {x-40} {y-20} 0) (unit 1)
    (property "Reference" "U8" (at 0 2 0)) (property "Value" "TPS62A01_QRY" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U8") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "ap2112k_3_3") (at {x-40} {y} 0) (unit 1)
    (property "Reference" "U9" (at 0 2 0)) (property "Value" "AP2112K-3.3" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U9") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "tps62a02") (at {x-40} {y+20} 0) (unit 1)
    (property "Reference" "U10" (at 0 2 0)) (property "Value" "TPS62A02_CORE" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U10") (unit 1)))))""")

    # 3× M.2 NVMe slots — below FPGA
    for slot in range(3):
        mx = x + 60 + slot * 50
        instances.append(f"""(symbol (lib_id "m2_mkey_nvme") (at {mx} {y-60} 0) (unit 1)
    (property "Reference" "J{7+slot}" (at 0 2 0)) (property "Value" "NVMe_{slot}" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "J{7+slot}") (unit 1)))))""")

    # 100MHz PCIe reference clock oscillator
    instances.append(f"""(symbol (lib_id "100mhz_diff") (at {x+90} {y-40} 0) (unit 1)
    (property "Reference" "Y3" (at 0 2 0)) (property "Value" "100MHz_DIFF" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "Y3") (unit 1)))))""")

    # NVMe 3.3V 3A buck
    instances.append(f"""(symbol (lib_id "tps54331") (at {x+60} {y-40} 0) (unit 1)
    (property "Reference" "U16" (at 0 2 0)) (property "Value" "TPS54331_NVMe" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U16") (unit 1)))))""")

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

    # ── Index Pico subsystem ──

    # Index RP2354B — right side, dedicated to index block I/O
    instances.append(f"""(symbol (lib_id "rp2354b") (at {x+180} {y+60} 0) (unit 1)
    (property "Reference" "U11" (at 0 2 0)) (property "Value" "RP2354B_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U11") (unit 1)))))""")

    # Index pico PSRAM
    instances.append(f"""(symbol (lib_id "ly68l6400") (at {x+220} {y+60} 0) (unit 1)
    (property "Reference" "U12" (at 0 2 0)) (property "Value" "LY68L6400_IDX" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U12") (unit 1)))))""")

    # ── GbE + PoE (FPGA-owned, replaces W5500) ──

    # RTL8221B GbE PHY — SGMII to ECP5 SerDes CH3
    instances.append(f"""(symbol (lib_id "rtl8211f") (at {x+180} {y+100} 0) (unit 1)
    (property "Reference" "U13" (at 0 2 0)) (property "Value" "RTL8221B" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U13") (unit 1)))))""")

    # 25MHz crystal for RTL8221B
    instances.append(f"""(symbol (lib_id "25mhz") (at {x+210} {y+100} 0) (unit 1)
    (property "Reference" "Y2" (at 0 2 0)) (property "Value" "25MHz" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "Y2") (unit 1)))))""")

    # TPS23753A PoE PD controller
    instances.append(f"""(symbol (lib_id "tps23753a") (at {x+250} {y+120} 0) (unit 1)
    (property "Reference" "U17" (at 0 2 0)) (property "Value" "TPS23753A_PoE" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "query_planner" (path "/{make_uuid()}" (reference "U17") (unit 1)))))""")

    # RJ45 PoE MagJack with center taps
    instances.append(f"""(symbol (lib_id "rj45_poe_magjack") (at {x+250} {y+100} 0) (unit 1)
    (property "Reference" "J5" (at 0 2 0)) (property "Value" "RJ45_PoE" (at 0 -2 0))
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

    # Net labels — FPGA↔SRAM, FPGA↔RPs, PCIe/NVMe, upstream/downstream SPI, Ethernet
    netlabels = []
    bus_nets = [
        # Upstream SPI
        "UP_SPI_SCK", "UP_SPI_MOSI", "UP_SPI_MISO", "UP_SPI_CS",
        # FPGA↔Query RP SPI
        "RP_SPI_SCK", "RP_SPI_MOSI", "RP_SPI_MISO", "RP_SPI_CS",
        "RP_IRQ", "RP_BUSY",
        # Downstream SPI (legacy expansion)
        "DN_SPI_SCK", "DN_SPI_MOSI", "DN_SPI_MISO", "DN_SPI_CS",
        # FPGA↔Index RP SPI (index path)
        "IDX_SPI_SCK", "IDX_SPI_MOSI", "IDX_SPI_MISO", "IDX_SPI_CS",
        "IDX_IRQ",
        # PCIe SerDes (3× NVMe channels)
        "PCIE_REFCLK_P", "PCIE_REFCLK_N",
    ]
    for ch in range(3):
        bus_nets += [
            f"NVME{ch}_TX_P", f"NVME{ch}_TX_N",
            f"NVME{ch}_RX_P", f"NVME{ch}_RX_N",
            f"NVME{ch}_PERST_N", f"NVME{ch}_CLKREQ_N",
        ]
    bus_nets += [
        # GbE PHY management (FPGA → RTL8221B)
        "PHY_MDC", "PHY_MDIO", "PHY_RST_N", "PHY_INT_N",
        # SGMII (SerDes CH3 → RTL8221B) — dedicated SerDes pins, listed for netlist
        "SGMII_TXP", "SGMII_TXN", "SGMII_RXP", "SGMII_RXN",
        # PoE power
        "POE_VIN", "POE_VOUT",
        # Power rails
        "VCC_3V3", "VCC_1V1", "VCC_NVME_3V3", "DVDD_QRY", "DVDD_IDX", "GND",
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
    (title "PicoWAL Query Processor v{VERSION} — ECP5-5G + 3× NVMe RAID + GbE TOE + PoE")
    (comment 1 "ECP5-5G-45F: SerDes CH0-2 → NVMe, CH3 → SGMII GbE PHY. HW TCP/IP offload in FPGA.")
    (comment 2 "FAST PATH: addr[52]=0 → FPGA reads NVMe directly (zero copy, zero pico)")
    (comment 3 "QUERY PATH: addr[52]=1 → FPGA queues in SRAM FIFO → query pico issues copy cmds")
    (comment 4 "PoE 802.3at (25.5W) powers entire board. RTL8221B GbE PHY + FPGA TOE = no W5500."))
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


def gen_pcb(board_name, bom, board_w=130, board_h=80):
    footprints = []
    # Layout: power left, MCUs center-left, FPGA center, SRAM right-of-FPGA,
    # M.2 NVMe slots across bottom, Ethernet+RJ45 right edge
    layout = {
        "query_planner": {
            # FPGA + memory (center)
            "U1": ("Package_BGA:BGA-381_17x17mm_P0.8mm", "ECP5-5G-45F", 50, 25),
            "U2": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_QRY", 20, 25),
            "U3": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_A", 72, 10),
            "U4": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_B", 72, 40),
            "U5": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_QRY", 20, 42),
            "U6": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_QRY", 8, 10),
            "U7": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_CFG", 50, 42),
            # Power regulators (left edge)
            "U8": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_QRY", 5, 20),
            "U9": ("Package_TO_SOT_SMD:SOT-23-5", "AP2112K_3V3", 5, 28),
            "U10": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A02_CORE", 5, 34),
            # Index pico subsystem (right half)
            "U11": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_IDX", 92, 25),
            "U12": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_IDX", 92, 42),
            "U13": ("Package_DFN_QFN:QFN-56_7x7mm_P0.4mm", "RTL8221B", 108, 25),
            "U14": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_IDX", 92, 10),
            "U15": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_IDX", 85, 20),
            # NVMe power
            "U16": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "TPS54331_NVMe", 5, 60),
            # PoE PD controller
            "U17": ("Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", "TPS23753A_PoE", 120, 40),
            # 3× M.2 NVMe slots (bottom row)
            "J7": ("Connector_M2:M2_M_Key_22x80", "NVMe_0", 25, 68),
            "J8": ("Connector_M2:M2_M_Key_22x80", "NVMe_1", 55, 68),
            "J9": ("Connector_M2:M2_M_Key_22x80", "NVMe_2", 85, 68),
            # Crystals / oscillators
            "Y1": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "12MHz", 30, 15),
            "Y2": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "25MHz", 108, 15),
            "Y3": ("Oscillator:Oscillator_SMD_3225-4Pin_3.2x2.5mm", "100MHz", 50, 15),
            # Connectors
            "J1": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "UP", 0, 25),
            "J2": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "DN", 0, 45),
            "J3": ("Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", "UART", 120, 50),
            "J4": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_QRY", 30, 50),
            "J5": ("Connector_RJ45:RJ45_Amphenol_ARJM11C7", "RJ45", 120, 25),
            "J6": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_IDX", 100, 50),
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
    print("\n  Pin Budget — ECP5-5G-45F CABGA381 (~205 user I/O + 4× SerDes)")
    print("  " + "─" * 60)
    pins = [
        ("Upstream SPI (slave)", 4),
        ("Downstream SPI (master, legacy/expansion)", 4),
        ("Query pico 8-bit bus (D[7:0]+RDY+ACK+DIR+SOF+EOF+SOCK[1:0])", 15),
        ("Index pico 8-bit bus (D[7:0]+RDY+ACK+DIR+SOF+EOF+SOCK[1:0])", 15),
        ("SRAM bank A (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("SRAM bank B (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("Config flash SPI (MSPI boot)", 4),
        ("DONE/INITN/PROGRAMN", 3),
        ("NVMe PERST# x 3", 3),
        ("NVMe CLKREQ# x 3", 3),
        ("RTL8221B PHY (MDC/MDIO/RST/INT)", 4),
        ("Status LEDs", 4),
        ("-- SerDes (dedicated, not I/O) --", 0),
        ("  PCIe TX+/- x 4 channels", 0),
        ("  PCIe RX+/- x 4 channels", 0),
        ("  REFCLK+/- x 2 (per DCU)", 0),
    ]
    total = 0
    for name, n in pins:
        if n > 0:
            print(f"    {name:52s} {n:3d}")
            total += n
        else:
            print(f"    {name}")
    print(f"    {'─'*52} {'─'*3}")
    print(f"    {'TOTAL (general purpose I/O)':52s} {total:3d} / 205  ({'OK' if total <= 205 else 'OVER'})")
    print(f"    SerDes channels: 3 NVMe (CH0-2) + 1 GbE/2.5GbE (CH3) = all 4 used")


def power_budget_report():
    print("\n  Power Budget — Single card, PoE 802.3at (25.5W available)")
    print("  " + "─" * 60)
    rails = [
        ("ECP5-5G-45F core (1.1V, 300mA)", 0.330),
        ("ECP5-5G-45F I/O (3.3V, 60mA)", 0.198),
        ("ECP5-5G-45F aux (2.5V, 20mA)", 0.050),
        ("ECP5-5G-45F SerDes TX (1.1V, 80mA×4ch)", 0.352),
        ("ECP5-5G-45F SerDes RX (1.1V, 60mA×4ch)", 0.264),
        ("FPGA TOE logic (~8K LUTs extra)", 0.050),
        ("RP2354B #1 QRY DVDD (1.1V, 100mA)", 0.110),
        ("RP2354B #1 QRY IOVDD (3.3V, 50mA)", 0.165),
        ("RP2354B #2 IDX DVDD (1.1V, 100mA)", 0.110),
        ("RP2354B #2 IDX IOVDD (3.3V, 50mA)", 0.165),
        ("2× IS61WV25616BLL (3.3V, 40mA each)", 0.264),
        ("2× LY68L6400 PSRAM (3.3V, 25mA each)", 0.165),
        ("RTL8221B 2.5GbE PHY (1.0V+3.3V, ~800mW)", 0.800),
        ("Flash chips ×3 (3.3V, 60mA total)", 0.198),
        ("100MHz LVDS oscillator (3.3V, 30mA)", 0.099),
        ("25MHz crystal osc (negligible)", 0.010),
        ("PoE PD controller (TPS23753A overhead)", 0.150),
        ("OC headroom (+25% on both DVDD)", 0.055),
        ("── NVMe Drives ──", 0),
        ("NVMe SSD ×3 (3.3V, ~1A each typical)", 9.900),
    ]
    subtotal_board = 0
    total = 0
    for name, w in rails:
        if w > 0:
            print(f"    {name:52s} {w:.3f}W")
            total += w
            if "NVMe SSD" not in name:
                subtotal_board += w
        else:
            print(f"    {name}")
    print(f"    {'─'*52} {'─'*6}")
    print(f"    {'Board logic (excl. NVMe drives)':52s} {subtotal_board:.3f}W")
    print(f"    {'TOTAL (with 3× NVMe drives)':52s} {total:.3f}W")


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
        f.write(gen_pcb("query_planner", QUERY_PLANNER_BOM, board_w=130, board_h=80))
    print("[OK] query_planner.kicad_pcb")

    # Summary
    print(f"\n{'='*70}")
    print(f" PicoWAL v{VERSION} — PoE 2.5GbE FPGA NVMe RAID Query Engine")
    print(f"{'='*70}")
    print()
    print(" Architecture: Deterministic — FPGA owns dataplane, picos own streams.")
    print()
    print("   ECP5-5G-45F        4x SerDes: 3x PCIe NVMe + 1x 2500BASE-X")
    print("   3x M.2 M-Key NVMe PCIe Gen1 x1 each (~250 MB/s per drive)")
    print("   RTL8221B           2.5GbE PHY via 2500BASE-X on SerDes CH3")
    print("   FPGA TOE           Hardware TCP/IP offload (~8K LUTs)")
    print("   RP2354B #1 (QRY)   Owns TCP streams, parses queries, streams results")
    print("   RP2354B #2 (IDX)   Owns all writes, WAL, index maintenance")
    print("   2x IS61WV25616BLL  FPGA-owned SRAM (query FIFO + page staging)")
    print("   2x LY68L6400       PSRAM per pico (working set only)")
    print("   TPS23753A + RJ45   PoE 802.3at -- self-powered from Ethernet")
    print("   Board:              130x80mm, 4-layer")
    print()
    print(" Pico bus: 8-bit parallel, RDY/ACK handshake, 15 pins per pico")
    print("   DATA[7:0] + RDY + ACK + DIR + SOF + EOF + SOCK[1:0]")
    print("   ~50 MB/s @ 50MHz -- saturates 2.5GbE easily")
    print("   PIO+DMA on pico side -- zero CPU overhead for data movement")
    print()
    print(" SerDes allocation:")
    print("   CH0 -> PCIe Gen1 x1 -> M.2 NVMe slot 0 (data shard)")
    print("   CH1 -> PCIe Gen1 x1 -> M.2 NVMe slot 1 (data shard)")
    print("   CH2 -> PCIe Gen1 x1 -> M.2 NVMe slot 2 (WAL + indexes)")
    print("   CH3 -> 2500BASE-X  -> RTL8221B -> RJ45 (2.5GbE + PoE)")
    print("   100MHz LVDS ref clock shared via fanout buffer")
    print()
    print(" Deterministic data paths:")
    print("   PATH 1 (Direct read, ~10us): Client -> TCP -> FPGA -> NVMe -> TCP")
    print("       FPGA parses fixed header, reads block by address, returns via TCP")
    print("       Zero pico involvement. Deterministic pipeline latency.")
    print("   PATH 2 (Query, ~100us-10ms): Client -> TCP -> Pico A -> FPGA -> NVMe")
    print("       Pico A parses query, requests blocks, filters, streams results")
    print("   PATH 3 (Write): Client -> TCP -> Pico B -> WAL -> index update")
    print("       All writes serialized through Pico B. No contention ever.")
    print()
    print(" Address format: [63:53]=tenant [52]=INDEX [51:42]=card [41:0]=block")
    print("   addr[52]=0  DATA   -> FPGA reads NVMe directly (zero copy)")
    print("   addr[52]=1  INDEX  -> FPGA queues -> query pico processes")
    print()
    print(" Pico command interface (8-bit parallel bus, frame-based):")
    print("   SOF byte = command opcode, payload follows, EOF terminates")
    print("   CMD_FIFO_POP   0x10  Query pico reads next descriptor from FIFO")
    print("   CMD_IDX_READ   0x01  Read index block from NVMe via FPGA")
    print("   CMD_IDX_WRITE  0x05  Write index block to NVMe via FPGA")
    print("   CMD_COPY_OUT   0x02  FPGA reads data block -> streams to TCP")
    print("   CMD_MULTI_START 0x03 Begin multi-block gather")
    print("   CMD_MULTI_END  0x04  Flush gathered response")
    print("   CMD_NOTIFY_ACK 0x11  Index pico acks write notification")
    print()

    print(" BOM:")
    total = sum(p["cost"] for p in QUERY_PLANNER_BOM) + 8.0 + 3.50
    for part in QUERY_PLANNER_BOM:
        print(f"   {part['ref']:6s}  {part['value']:24s}  £{part['cost']:.2f}  {part['desc']}")
    print(f"   {'PCB':6s}  {'4-layer 130×80mm':24s}  £8.00")
    print(f"   {'MISC':6s}  {'Passives+bypass+fanout':24s}  £3.50")
    print(f"   {'─'*62}")
    print(f"   {'TOTAL':6s}  {'(excl. NVMe SSDs)':24s}  £{total:.2f}")
    print()

    print(" OC Configuration (both picos):")
    print("   TPS62A01 DVDD: R_FB1=100kΩ, R_FB2=91kΩ → 1.1V (stock)")
    print("   OC mode:       R_FB2=82kΩ → 1.15V | R_FB2=68kΩ → 1.25V")
    print("   RP2354B PLL:   stock 150MHz | OC target 200MHz")
    print("   Extra decoupling: 10μF + 100nF per DVDD pin")
    print()

    print(" Data flow:")
    print("   ┌─ FAST PATH (addr[52]=0) ─────────────────────────┐")
    print("   │ upstream SPI → FPGA → PCIe NVMe read/write       │")
    print("   │ zero copy, zero pico, FPGA drives PCIe directly  │")
    print("   └──────────────────────────────────────────────────┘")
    print("   ┌─ QUERY PATH (addr[52]=1) ────────────────────────┐")
    print("   │ upstream → FPGA SRAM FIFO → qry_irq              │")
    print("   │ query pico: FIFO_POP → IDX_READ → COPY_OUT       │")
    print("   │ FPGA does all data movement, pico just commands   │")
    print("   └──────────────────────────────────────────────────┘")
    print("   ┌─ INDEX UPDATE (on data writes) ──────────────────┐")
    print("   │ FPGA NVMe write + idx_irq                        │")
    print("   │ index pico: IDX_READ → update → IDX_WRITE        │")
    print("   │ indexes stored as blocks with addr[52]=1 on NVMe │")
    print("   └──────────────────────────────────────────────────┘")

    pin_budget_report()
    power_budget_report()

    print(f"\n{'='*70}")


if __name__ == "__main__":
    main()
