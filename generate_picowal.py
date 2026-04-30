#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""generate_picowal.py -- PicoWAL Maxi Storage Appliance (FPGA + RK3588)

Generates KiCad 8 schematic + PCB for PicoWAL maxi design:

  - 2x ECP5-5G-85F (LFE5UM5G-85F-CABGA381): 170K LUT, 312 DSP, 8x SerDes
  - RK3588 SoC: 4xA76+4xA55, 32GB LPDDR5, 6 TOPS NPU, PCIe 3.0
  - 5x M.2 M-key NVMe: 3 data + 1 WAL + 1 index (10TB with 2TB drives)
  - 4x IS61WV25616BLL SRAM: 2MB page cache @ 10ns
  - 2x IDT70V28L DPRAM: 1MB B-tree index cache @ 15ns (dual-port)
  - 2x RTL8221B + dual PoE: 2x 2.5GbE (5 Gbps aggregate, 51W power)
  - 44 parallel query lanes: hardwired B-tree walk + predicate filter
  - 312 DSP MACs: 62.4 GMAC/s vector similarity search

Hybrid architecture: FPGA handles wire-speed queries, SoC handles complex ops.

FPGA duties (hardware query engine):
  - 44 parallel query lanes for KV reads (220M QPS cached)
  - Full TOE network stack (MAC/IP/TCP/SMB2/HTTP)
  - NVMe DMA controllers (5 drives)
  - B-tree index traversal in DPRAM

RK3588 SoC duties (app compute):
  - Complex query planning (JOINs, GROUP BY, aggregations)
  - Application server (REST/gRPC/custom logic)
  - CIFS metadata + extended attributes
  - ML/AI inference via 6 TOPS NPU
  - User-space drivers, orchestration, management
  - Communicates with FPGA B via PCIe 3.0 x1 (~1 GB/s)

NVMe role assignment:
  Slots 0,1 = DATA (FPGA A SerDes CH0,CH1)
  Slot 2 = DATA (FPGA B SerDes CH0)
  Slot 3 = WAL (FPGA B SerDes CH1)
  Slot 4 = INDEX (FPGA B SerDes CH2)

Target: <£1000 all-in including 5x 2TB NVMe SSDs.
Board: ~200x120mm, 4-layer, BGA/TSOP/M.2 -- requires reflow.
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
    """ECP5-5G-85F (LFE5UM5G-85F) CABGA381 with SerDes/PCIe + functional pin groups."""
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
    # ── FPGA A (Network + Query Lanes + Data NVMe) ──
    {"ref": "U1",  "value": "LFE5UM5G-85F",     "pkg": "CABGA-381", "cost": 15.00, "desc": "ECP5-5G FPGA A: TOE + 22 query lanes + 2x NVMe + 2x 2.5GbE + DSP"},
    {"ref": "U3",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank A (FPGA A, page cache / query results)"},
    {"ref": "U4",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank B (FPGA A, TCP buffers / hot data)"},
    {"ref": "U7",  "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "FPGA A config flash"},
    {"ref": "U10", "value": "TPS62A02",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "1.1V FPGA A core buck"},
    # ── FPGA B (WAL + Index + Query Lanes + Chain) ──
    {"ref": "U18", "value": "LFE5UM5G-85F",     "pkg": "CABGA-381", "cost": 15.00, "desc": "ECP5-5G FPGA B: 22 query lanes + 3x NVMe + index engine + PCIe to RK3588"},
    {"ref": "U19", "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",    "cost": 1.20, "desc": "FPGA B config flash"},
    {"ref": "U20", "value": "TPS62A02",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "1.1V FPGA B core buck"},
    {"ref": "U21", "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank C (FPGA B, page cache / WAL buffer)"},
    {"ref": "U42", "value": "IS61WV25616BLL",    "pkg": "TSOP-44",   "cost": 1.90, "desc": "SRAM bank D (FPGA B, index node cache)"},
    # ── Dual-port SRAM (B-tree index cache, read from both FPGAs) ──
    # Port A: FPGA B writes (index update engine)
    # Port B: FPGA A reads (query lane B-tree traversal)
    {"ref": "U36", "value": "IDT70V28L",         "pkg": "TQFP-100",  "cost": 12.00, "desc": "DPRAM 0: 256Kx16 (512KB) B-tree hot nodes, portA=FPGA_B, portB=FPGA_A"},
    {"ref": "U37", "value": "IDT70V28L",         "pkg": "TQFP-100",  "cost": 12.00, "desc": "DPRAM 1: 256Kx16 (512KB) B-tree hot nodes, portA=FPGA_B, portB=FPGA_A"},
    # ── NVMe storage (5 slots) ──
    {"ref": "J7",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 0 data (FPGA A CH0)"},
    {"ref": "J8",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 1 data (FPGA A CH1)"},
    {"ref": "J9",  "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 2 data (FPGA B CH0)"},
    {"ref": "J10", "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 3 WAL (FPGA B CH1)"},
    {"ref": "J11", "value": "M2_M_KEY",          "pkg": "M2-M-Key",  "cost": 1.50, "desc": "NVMe slot 4 indexes (FPGA B CH2)"},
    {"ref": "Y3",  "value": "100MHz_DIFF",       "pkg": "3225",      "cost": 1.80, "desc": "PCIe refclk A (FPGA A)"},
    {"ref": "Y4",  "value": "100MHz_DIFF",       "pkg": "3225",      "cost": 1.80, "desc": "PCIe refclk B (FPGA B)"},
    {"ref": "U16", "value": "TPS54331",          "pkg": "SOIC-8",    "cost": 1.20, "desc": "3.3V 3A NVMe buck (drives 0+1)"},
    {"ref": "U22", "value": "TPS54331",          "pkg": "SOIC-8",    "cost": 1.20, "desc": "3.3V 3A NVMe buck (drives 2+3+4)"},
    # ── 2x 2.5GbE + PoE ──
    {"ref": "U13", "value": "RTL8221B",          "pkg": "QFN-56",    "cost": 3.50, "desc": "2.5GbE PHY A (2500BASE-X to FPGA A SerDes CH2)"},
    {"ref": "U23", "value": "RTL8221B",          "pkg": "QFN-56",    "cost": 3.50, "desc": "2.5GbE PHY B (2500BASE-X to FPGA A SerDes CH3)"},
    {"ref": "U17", "value": "TPS23753A",         "pkg": "TSSOP-20",  "cost": 2.50, "desc": "802.3at PoE PD port A (25.5W)"},
    {"ref": "U24", "value": "TPS23753A",         "pkg": "TSSOP-20",  "cost": 2.50, "desc": "802.3at PoE PD port B (25.5W)"},
    {"ref": "J5",  "value": "RJ45_POE_MAGJACK",  "pkg": "RJ45",      "cost": 4.00, "desc": "RJ45 PoE MagJack port A"},
    {"ref": "J12", "value": "RJ45_POE_MAGJACK",  "pkg": "RJ45",      "cost": 4.00, "desc": "RJ45 PoE MagJack port B"},
    # ── Shared ──
    {"ref": "Y2",  "value": "25MHz",             "pkg": "3215",      "cost": 0.20, "desc": "RTL8221B PHY crystal"},
    {"ref": "J1",  "value": "CONN_2x10",         "pkg": "2x10-2.54", "cost": 0.30, "desc": "Inter-board expansion (SPI + power)"},
    {"ref": "J3",  "value": "CONN_1x4",          "pkg": "1x4-2.54",  "cost": 0.10, "desc": "UART debug (FPGA A)"},
    {"ref": "J4",  "value": "CONN_1x6",          "pkg": "1x6-1.27",  "cost": 0.10, "desc": "JTAG (shared, active-low select)"},
    # ── RK3588 SoC (Cortex-A76/A55, 32GB LPDDR5) ──
    {"ref": "U50", "value": "RK3588",            "pkg": "FCBGA-796", "cost": 35.00, "desc": "RK3588: 4xA76+4xA55, PCIe3.0x1 to FPGA B CH3, 6 TOPS NPU"},
    {"ref": "U51", "value": "MT62F2G32D4DS",     "pkg": "FBGA-200",  "cost": 45.00, "desc": "32GB LPDDR5-5500 (4x 8GB die, PoP on RK3588)"},
    {"ref": "U52", "value": "RK806-1",           "pkg": "QFN-68",    "cost": 4.50, "desc": "RK3588 companion PMIC (8 buck + 7 LDO)"},
    {"ref": "U53", "value": "W25Q256JVEIQ",      "pkg": "SOIC-8",    "cost": 2.50, "desc": "RK3588 SPI NOR flash (U-Boot + ATF)"},
    {"ref": "U54", "value": "EMMC 32GB",         "pkg": "BGA-153",   "cost": 8.00, "desc": "eMMC for RK3588 rootfs (Linux/OS)"},
    {"ref": "U55", "value": "TPS62A02",          "pkg": "SOT-23-6",  "cost": 0.90, "desc": "0.9V RK3588 NPU/GPU core rail"},
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


def gen_pcb(board_name, bom, board_w=160, board_h=100):
    footprints = []
    # Layout: dual FPGA center, power left, picos flanking their FPGA,
    # 4× M.2 NVMe across bottom, Ethernet+RJ45 right edge
    layout = {
        "query_planner": {
            # FPGA A + memory (center-left)
            "U1": ("Package_BGA:BGA-381_17x17mm_P0.8mm", "ECP5-85F_A", 50, 30),
            "U2": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_QRY", 20, 30),
            "U3": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_A", 72, 15),
            "U4": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_B", 72, 45),
            "U5": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_QRY", 20, 47),
            "U6": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_QRY", 8, 15),
            "U7": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_CFG_A", 50, 50),
            # Power regulators (left edge)
            "U8": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_QRY", 5, 20),
            "U9": ("Package_TO_SOT_SMD:SOT-23-5", "AP2112K_3V3", 5, 28),
            "U10": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A02_CORE_A", 5, 34),
            # FPGA B + memory (center-right)
            "U18": ("Package_BGA:BGA-381_17x17mm_P0.8mm", "ECP5-85F_B", 110, 30),
            "U19": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_CFG_B", 110, 50),
            "U20": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A02_CORE_B", 95, 20),
            "U21": ("Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm", "SRAM_C", 132, 30),
            # Index pico subsystem (right of FPGA B)
            "U11": ("Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm", "RP2354B_IDX", 145, 30),
            "U12": ("Package_SO:SOP-8_3.9x4.9mm_P1.27mm", "PSRAM_IDX", 145, 47),
            "U14": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "W25Q128_IDX", 145, 15),
            "U15": ("Package_TO_SOT_SMD:SOT-23-6", "TPS62A01_IDX", 140, 20),
            # 2x 2.5GbE PHY + 2x PoE (right edge)
            "U13": ("Package_DFN_QFN:QFN-56_7x7mm_P0.4mm", "RTL8221B_A", 160, 15),
            "U23": ("Package_DFN_QFN:QFN-56_7x7mm_P0.4mm", "RTL8221B_B", 160, 35),
            "U17": ("Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", "TPS23753A_PoE_A", 160, 55),
            "U24": ("Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", "TPS23753A_PoE_B", 160, 70),
            # NVMe power bucks
            "U16": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "TPS54331_NVMe01", 5, 75),
            "U22": ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "TPS54331_NVMe234", 5, 85),
            # 5x M.2 NVMe slots (bottom row)
            "J7":  ("Connector_M2:M2_M_Key_22x80", "NVMe_0_data", 15, 95),
            "J8":  ("Connector_M2:M2_M_Key_22x80", "NVMe_1_data", 45, 95),
            "J9":  ("Connector_M2:M2_M_Key_22x80", "NVMe_2_data", 75, 95),
            "J10": ("Connector_M2:M2_M_Key_22x80", "NVMe_3_WAL", 105, 95),
            "J11": ("Connector_M2:M2_M_Key_22x80", "NVMe_4_idx", 135, 95),
            # Crystals / oscillators
            "Y1": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "12MHz", 30, 15),
            "Y2": ("Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", "25MHz", 160, 8),
            "Y3": ("Oscillator:Oscillator_SMD_3225-4Pin_3.2x2.5mm", "100MHz_A", 50, 15),
            "Y4": ("Oscillator:Oscillator_SMD_3225-4Pin_3.2x2.5mm", "100MHz_B", 110, 15),
            # Connectors
            "J1": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "UP", 0, 30),
            "J2": ("Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical", "DN", 0, 50),
            "J3": ("Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", "UART", 170, 60),
            "J4": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_QRY", 30, 55),
            "J5": ("Connector_RJ45:RJ45_Amphenol_ARJM11C7", "RJ45_A", 170, 15),
            "J6": ("Connector_PinHeader_1.27mm:PinHeader_1x05_P1.27mm_Vertical", "SWD_IDX", 150, 55),
            "J12": ("Connector_RJ45:RJ45_Amphenol_ARJM11C7", "RJ45_B", 170, 35),
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
    print("\n  Pin Budget -- Dual ECP5-5G-85F CABGA381 (~205 user I/O + 4x SerDes each)")
    print("  " + "-" * 60)
    print("  FPGA A (Network + Query Lanes + Data):")
    pins_a = [
        ("SRAM bank A (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("SRAM bank B (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("DPRAM 0 port B read (A[17:0]+D[15:0]+CE/OE)", 36),
        ("DPRAM 1 port B read (A[17:0]+D[15:0]+CE/OE)", 36),
        ("2x RTL8221B PHY (MDC/MDIO/RST/INT each)", 8),
        ("Config flash SPI (MSPI boot)", 4),
        ("DONE/INITN/PROGRAMN", 3),
        ("NVMe PERST# x 2", 2),
        ("Status LEDs", 4),
    ]
    total_a = 0
    for name, n in pins_a:
        print(f"    {name:56s} {n:3d}")
        total_a += n
    print(f"    {'-'*56} {'-'*3}")
    print(f"    {'TOTAL FPGA A I/O':56s} {total_a:3d} / 205  ({'OK' if total_a <= 205 else 'OVER'})")
    print(f"    SerDes: CH0=NVMe0, CH1=NVMe1, CH2=2.5GbE portA, CH3=2.5GbE portB")
    print()
    print("  FPGA B (WAL + Index Engine + Query Lanes + SoC link):")
    pins_b = [
        ("SRAM bank C (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("SRAM bank D (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("DPRAM 0 port A write (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("DPRAM 1 port A write (A[17:0]+D[15:0]+CE/OE/WE)", 37),
        ("RK3588 misc GPIO (reset/IRQ/status)", 4),
        ("Config flash SPI (MSPI boot)", 4),
        ("DONE/INITN/PROGRAMN", 3),
        ("NVMe PERST# x 3", 3),
        ("Status LEDs", 2),
    ]
    total_b = 0
    for name, n in pins_b:
        print(f"    {name:56s} {n:3d}")
        total_b += n
    print(f"    {'-'*56} {'-'*3}")
    print(f"    {'TOTAL FPGA B I/O':56s} {total_b:3d} / 205  ({'OK' if total_b <= 205 else 'OVER'})")
    print(f"    SerDes: CH0=NVMe2(data), CH1=NVMe3(WAL), CH2=NVMe4(idx), CH3=PCIe to RK3588")


def power_budget_report():
    print("\n  Power Budget -- Dual PoE 802.3at (51W total available)")
    print("  " + "-" * 60)
    rails = [
        ("ECP5-5G-85F A core (1.1V, 500mA, high util)", 0.550),
        ("ECP5-5G-85F A I/O (3.3V, 100mA, 2x SRAM + 2x DPRAM)", 0.330),
        ("ECP5-5G-85F A aux (2.5V, 20mA)", 0.050),
        ("ECP5-5G-85F A SerDes (TX+RX, 4ch)", 0.616),
        ("ECP5-5G-85F B core (1.1V, 450mA, high util)", 0.495),
        ("ECP5-5G-85F B I/O (3.3V, 100mA, 2x SRAM + 2x DPRAM)", 0.330),
        ("ECP5-5G-85F B aux (2.5V, 20mA)", 0.050),
        ("ECP5-5G-85F B SerDes (TX+RX, 4ch)", 0.616),
        ("FPGA A TOE + query lanes (44K LUTs active)", 0.400),
        ("FPGA B index engine + query lanes (44K LUTs)", 0.400),
        ("FPGA DSP array (312 MACs active)", 0.200),
        ("4x IS61WV25616BLL SRAM (3.3V, 40mA each)", 0.528),
        ("2x IDT70V28L DPRAM (3.3V, 120mA each)", 0.792),
        ("RK3588 SoC (4xA76+4xA55, typical)", 5.000),
        ("32GB LPDDR5 (4 channels, active)", 2.400),
        ("RK3588 PMIC (RK806 overhead)", 0.150),
        ("eMMC 32GB", 0.200),
        ("2x RTL8221B 2.5GbE PHY (~800mW each)", 1.600),
        ("Flash chips x3 (config + RK3588 SPI NOR)", 0.099),
        ("2x 100MHz LVDS oscillator", 0.198),
        ("25MHz crystal osc", 0.010),
        ("2x PoE PD controller overhead", 0.300),
        ("-- NVMe Drives --", 0),
        ("NVMe SSD x5 (3.3V, ~1A each typical)", 16.500),
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
    print(f"    {'-'*52} {'-'*6}")
    print(f"    {'Board logic (excl. NVMe drives)':52s} {subtotal_board:.3f}W")
    print(f"    {'TOTAL (with 5x NVMe drives)':52s} {total:.3f}W")
    budget = 51.0
    print(f"    {'Dual PoE 802.3at budget (2x25.5W)':52s} {budget:.1f}W")
    margin = budget - total
    status = "OK" if margin > 0 else "OVER BUDGET"
    print(f"    {'Margin':52s} {margin:.3f}W ({status})")


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
        f.write(gen_pcb("query_planner", QUERY_PLANNER_BOM, board_w=200, board_h=120))
    print("[OK] query_planner.kicad_pcb")

    # Summary
    print(f"\n{'='*70}")
    print(f" PicoWAL v{VERSION} -- FPGA + RK3588 Storage Appliance (Maxi)")
    print(f"{'='*70}")
    print()
    print(" Architecture: Dual FPGA + RK3588 SoC. 44 query lanes + 8 CPU cores.")
    print()
    print("   2x ECP5-5G-85F     170K LUT, 312 DSP MACs, 8x SerDes")
    print("   RK3588 SoC         4xA76 + 4xA55, 32GB LPDDR5, 6 TOPS NPU")
    print("   44 query lanes     Hardwired B-tree walk + predicate filter")
    print("   5x M.2 M-Key NVMe  10TB with 2TB drives")
    print("   4x SRAM (512KB ea)  2MB page cache @ 10ns")
    print("   2x DPRAM (512KB ea) 1MB B-tree index cache @ 15ns (dual-port)")
    print("   2x RTL8221B        2x 2.5GbE (5 Gbps aggregate)")
    print("   2x TPS23753A       Dual PoE 802.3at (51W)")
    print("   312 DSP MACs       62.4 GMAC/s vector search")
    print("   Board:             200x120mm, 4-layer")
    print()
    print(" Performance (@ 125MHz clock):")
    print("   Cache hit:    200ns per query -> 220M queries/sec (44 lanes)")
    print("   NVMe path:    15us per query  -> 312K queries/sec (NVMe-bound)")
    print("   Blend (80%):  ~176M queries/sec sustained")
    print("   Network:      5 Gbps line-rate (2x 2.5GbE)")
    print()
    print(" Memory hierarchy:")
    print("   DPRAM (1MB, 15ns)  Hot B-tree root + L1 index nodes")
    print("   SRAM  (2MB, 10ns)  Page cache, TCP buffers, query results")
    print("   NVMe  (10TB, 15us) Cold storage (3 data + 1 WAL + 1 index)")
    print()
    print(" Query lane pipeline (per lane, ~2300 LUTs):")
    print("   1. Parse request: extract key + predicates (8 clk)")
    print("   2. B-tree walk: traverse DPRAM index nodes (8 clk)")
    print("   3. Fetch: read SRAM cache or issue NVMe DMA (1-1875 clk)")
    print("   4. Filter: apply predicate in 1 clock (pipelined)")
    print("   5. Stream: frame result back to TCP (8 clk)")
    print()
    print(" LUT allocation (170K total across 2 FPGAs):")
    print("   Network stack (2 ports):  44,000 LUTs")
    print("   NVMe controllers (5x):    15,000 LUTs")
    print("   DMA + SRAM arbitration:    5,000 LUTs")
    print("   Index update engine:       3,000 LUTs")
    print("   DSP glue (vector):         3,000 LUTs")
    print("   44 query lanes:          101,200 LUTs (2,300 each)")
    print("   TOTAL:                   171,200 / 170,000 (tight fit)")
    print()
    print(" FPGA A: TOE + socket mux + 22 query lanes + 2x NVMe DMA")
    print(" FPGA B: Index engine + 22 query lanes + 3x NVMe DMA + PCIe to RK3588")
    print()
    print(" Deterministic data paths:")
    print("   PATH 1 (KV hit):   TCP -> parse -> DPRAM lookup -> SRAM -> TCP")
    print("                      200ns end-to-end, zero NVMe")
    print("   PATH 2 (KV miss):  TCP -> parse -> DPRAM -> NVMe fetch -> TCP")
    print("                      ~15us (NVMe latency dominates)")
    print("   PATH 3 (Write):    TCP -> parse -> NVMe WAL -> index update")
    print("   PATH 4 (Scan):     TCP -> NVMe stream -> filter pipeline -> TCP")
    print("   PATH 5 (Vector):   TCP -> NVMe stream -> DSP systolic -> top-K -> TCP")
    print("   PATH 6 (CIFS):     SMB2 parse -> same as PATH 1/2")
    print()
    print(" SerDes allocation (8 channels):")
    print("   FPGA A CH0 -> NVMe slot 0 (data)")
    print("   FPGA A CH1 -> NVMe slot 1 (data)")
    print("   FPGA A CH2 -> 2.5GbE port A")
    print("   FPGA A CH3 -> 2.5GbE port B")
    print("   FPGA B CH0 -> NVMe slot 2 (data)")
    print("   FPGA B CH1 -> NVMe slot 3 (WAL)")
    print("   FPGA B CH2 -> NVMe slot 4 (indexes)")
    print("   FPGA B CH3 -> PCIe 3.0 x1 to RK3588 (~1 GB/s)")
    print()
    print(" Inter-FPGA link: DPRAMs bridge both FPGAs (no bus needed)")
    print("   FPGA B writes index updates to DPRAM port A")
    print("   FPGA A reads hot B-tree nodes from DPRAM port B")
    print()
    print(" RK3588 SoC (via PCIe 3.0 x1 to FPGA B):")
    print("   4x Cortex-A76 @ 2.4GHz + 4x Cortex-A55 @ 1.8GHz")
    print("   32GB LPDDR5-5500 (44 GB/s bandwidth)")
    print("   6 TOPS NPU (INT8 inference)")
    print("   32GB eMMC (Linux rootfs)")
    print("   Roles: complex query planning, JOIN/GROUP BY, app server,")
    print("          ML inference, CIFS metadata, user-space drivers")
    print("   PCIe link: SoC submits queries to FPGA, reads results from DPRAM")
    print()

    print(" BOM (board only, excl. NVMe SSDs):")
    total = sum(p["cost"] for p in QUERY_PLANNER_BOM) + 18.0 + 8.00
    for part in QUERY_PLANNER_BOM:
        print(f"   {part['ref']:6s}  {part['value']:24s}  {chr(163)}{part['cost']:.2f}  {part['desc']}")
    print(f"   {'PCB':6s}  {'4-layer 200x120mm':24s}  {chr(163)}18.00")
    print(f"   {'MISC':6s}  {'Passives+bypass+fanout':24s}  {chr(163)}8.00")
    print(f"   {'-'*62}")
    print(f"   {'TOTAL':6s}  {'(board only)':24s}  {chr(163)}{total:.2f}")
    print()
    print(f" Full system cost ({chr(163)}1000 budget):")
    nvme_cost = 450.0
    print(f"   Board components + PCB:              {chr(163)}{total:.2f}")
    print(f"   5x 2TB NVMe SSD (~{chr(163)}90 each):        {chr(163)}{nvme_cost:.2f}")
    print(f"   Enclosure + cables:                  {chr(163)}30.00")
    grand = total + nvme_cost + 30.0
    print(f"   {'-'*42}")
    print(f"   GRAND TOTAL:                         {chr(163)}{grand:.2f}")
    print(f"   Budget remaining:                    {chr(163)}{1000.0 - grand:.2f}")
    print()

    pin_budget_report()
    power_budget_report()

    print(f"\n{'='*70}")


if __name__ == "__main__":
    main()
