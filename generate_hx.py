#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""generate_hx.py -- PicoWAL HX: iCE40HX8K Pure-Logic Query Engine

Minimal, elegant PicoScript execution engine using only commodity parts:

  - iCE40HX8K-TQ144: 7680 LUTs, 206 GPIO, 0 DSP -- runs ALL non-DSP opcodes
  - Parallel async SRAM: 512KB page cache (10ns access, zero wait states)
  - SD card over SPI: bulk card storage (WAL + data)
  - W5100S over SPI: hardware TCP/IP (4 sockets, no TOE needed in FPGA)

Optional ECP5 DSP coprocessor daughtercard:
  - Plugs in via 20-pin header (SPI + parallel dispatch bus)
  - Handles: MUL, DIV, DSP sub-ops (MATMUL, SOFTMAX, etc.)
  - Without it: MUL/DIV done in soft logic (slower), DSP ops return error
  - With it: full AI acceleration at hardware speed

Two product variants:
  - PicoWAL HX:     base board, £25-30, ~50K QPS cached
  - PicoWAL HX-AI:  base + ECP5-25F coprocessor, £50-60, + MATMUL/SOFTMAX

Key insight: W5100S does TCP/IP in hardware, so the FPGA needs ZERO network
logic. All 7680 LUTs are available for PicoScript execution + SRAM control.

Opcodes handled natively (in LUTs, single cycle):
  NOOP, LOAD, SAVE, PIPE, ADD, SUB, INC, JUMP, BRANCH, CALL, RETURN, WAIT, RAISE
  = 13 of 16 opcodes at full speed

Opcodes requiring ECP5 coprocessor (or soft fallback):
  MUL (soft: 8 cycles shift-add, ECP5: 1 cycle)
  DIV (soft: 32 cycles, ECP5: 3 cycles)
  DSP (ECP5 only, error without coprocessor)

Board: ~60×40mm, 2-layer PCB, all through-hole/TQFP -- hand-solderable!
Power: 3.3V USB or PoE via external splitter. <500mW total.
"""

import os, uuid

VERSION = "1.0"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "picowal_hx")

_uuid_counter = 0
def make_uuid():
    global _uuid_counter
    _uuid_counter += 1
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"picowal_hx.{_uuid_counter}"))


# ═══════════════════════════════════════════════════════════════════════
# BOM -- Base Board (HX)
# ═══════════════════════════════════════════════════════════════════════

BASE_BOM = [
    # FPGA
    {"ref": "U1", "value": "iCE40HX8K-TQ144", "pkg": "TQFP-144", "cost": 5.50,
     "desc": "Lattice iCE40HX8K: 7680 LUT, 206 I/O, 128Kbit BRAM"},
    {"ref": "U2", "value": "W25Q32JVSIQ",      "pkg": "SOIC-8",   "cost": 0.40,
     "desc": "FPGA config flash (32Mbit SPI, stores bitstream)"},
    {"ref": "Y1", "value": "12MHz",             "pkg": "HC49",     "cost": 0.15,
     "desc": "Master clock (PLL to 48/96MHz internally)"},

    # SRAM (parallel async)
    {"ref": "U3", "value": "IS61WV25616BLL",   "pkg": "TSOP-44",  "cost": 1.90,
     "desc": "512KB SRAM (256K×16, 10ns async) -- card page cache"},

    # Network
    {"ref": "U4", "value": "W5100S",           "pkg": "LQFP-80",  "cost": 1.80,
     "desc": "WIZnet W5100S: HW TCP/IP, 4 sockets, SPI mode"},
    {"ref": "J2", "value": "RJ45_MAGJACK",    "pkg": "RJ45",     "cost": 2.00,
     "desc": "RJ45 with integrated magnetics (10/100 Ethernet)"},

    # Storage
    {"ref": "J3", "value": "SD_CARD_SLOT",     "pkg": "SDCARD",   "cost": 0.60,
     "desc": "Micro SD card slot (SPI mode)"},

    # Power
    {"ref": "U5", "value": "AMS1117-3.3",     "pkg": "SOT-223",  "cost": 0.10,
     "desc": "3.3V LDO (from 5V USB)"},
    {"ref": "J1", "value": "USB_C_PWR",       "pkg": "USB-C",    "cost": 0.30,
     "desc": "USB-C power input (5V, VBUS only)"},

    # Coprocessor header
    {"ref": "J4", "value": "CONN_2x10",       "pkg": "2x10-2.54", "cost": 0.20,
     "desc": "ECP5 coprocessor header (SPI + 8-bit dispatch + IRQ)"},

    # Programming
    {"ref": "J5", "value": "CONN_2x5",        "pkg": "2x5-1.27",  "cost": 0.15,
     "desc": "JTAG/SPI programming header"},

    # Passives (bulk estimate)
    {"ref": "C*", "value": "DECOUPLING",      "pkg": "0402",      "cost": 0.50,
     "desc": "Decoupling caps (20× 100nF + 4× 10uF)"},
    {"ref": "R*", "value": "RESISTORS",       "pkg": "0402",      "cost": 0.20,
     "desc": "Pull-ups, current limit, etc. (~10 total)"},
]


# ═══════════════════════════════════════════════════════════════════════
# BOM -- Optional ECP5 DSP Coprocessor Daughtercard
# ═══════════════════════════════════════════════════════════════════════

COPROCESSOR_BOM = [
    {"ref": "U1", "value": "LFE5U-25F",       "pkg": "CABGA-256", "cost": 6.00,
     "desc": "ECP5-25F: 24K LUT, 56 DSP MACs (NO SerDes needed)"},
    {"ref": "U2", "value": "W25Q32JVSIQ",     "pkg": "SOIC-8",    "cost": 0.40,
     "desc": "ECP5 config flash (32Mbit SPI)"},
    {"ref": "Y1", "value": "25MHz",            "pkg": "3225",      "cost": 0.20,
     "desc": "ECP5 reference clock"},
    {"ref": "J1", "value": "CONN_2x10",       "pkg": "2x10-2.54", "cost": 0.20,
     "desc": "Mating header (plugs into base board J4)"},
    {"ref": "C*", "value": "DECOUPLING",      "pkg": "0402",      "cost": 0.30,
     "desc": "Decoupling (10× 100nF + 2× 10uF)"},
]


# ═══════════════════════════════════════════════════════════════════════
# Pin Budget -- iCE40HX8K-TQ144 (107 user I/O available)
# ═══════════════════════════════════════════════════════════════════════

def pin_budget_report():
    print("\n  Pin Budget -- iCE40HX8K-TQ144 (107 user I/O)")
    print("  " + "-" * 60)
    pins = [
        ("SRAM: A[17:0]",                       18),
        ("SRAM: D[15:0]",                       16),
        ("SRAM: CE#/OE#/WE#/LB#/UB#",           5),
        ("W5100S SPI: MOSI/MISO/SCK/CS#",        4),
        ("W5100S: INT#/RST#",                    2),
        ("SD card SPI: MOSI/MISO/SCK/CS#",       4),
        ("SD card: DETECT",                      1),
        ("Config flash SPI: MOSI/MISO/SCK/CS#",  4),
        ("ECP5 coprocessor: SPI (shared bus)",   0),  # reuses config SPI
        ("ECP5 coprocessor: D[7:0] dispatch",    8),
        ("ECP5 coprocessor: REQ/ACK/IRQ",        3),
        ("JTAG: TDI/TDO/TCK/TMS",               4),
        ("Clock input",                          1),
        ("Status LEDs",                          3),
        ("CRESET_B/CDONE",                       2),
    ]
    total = sum(n for _, n in pins)
    for name, n in pins:
        print(f"    {name:48s} {n:3d}")
    print(f"    {'-'*48} {'-'*3}")
    print(f"    {'TOTAL':48s} {total:3d} / 107")
    spare = 107 - total
    print(f"    {'Spare I/O':48s} {spare:3d}")
    status = "OK" if total <= 107 else "OVER"
    print(f"    Status: {status} ({spare} pins free for expansion)")
    return total <= 107


# ═══════════════════════════════════════════════════════════════════════
# LUT Budget -- iCE40HX8K (7680 LUTs, 128Kbit BRAM)
# ═══════════════════════════════════════════════════════════════════════

def lut_budget_report():
    print("\n  LUT Budget -- iCE40HX8K (7680 LUTs)")
    print("  " + "-" * 60)
    blocks = [
        ("PicoScript decoder (4-bit opcode, 13 ops)",     150),
        ("Register file (16×32, dual-port via BRAM)",       0),  # uses BRAM
        ("ALU: ADD/SUB/INC (32-bit)",                     200),
        ("ALU: MUL soft (shift-add, 8 cycles)",           300),
        ("ALU: DIV soft (restoring, 32 cycles)",          350),
        ("Branch comparator (10 conditions)",             180),
        ("Program counter + call stack (8 deep)",         120),
        ("SRAM controller (async, zero wait state)",      250),
        ("SPI master: W5100S (8MHz, mode 0)",             200),
        ("SPI master: SD card (25MHz, mode 0)",           200),
        ("Card address mapper (imm16 -> sector)",         150),
        ("Connection context manager (4 contexts)",       300),
        ("PIPE engine (SRAM -> W5100S TX buffer)",        250),
        ("Coprocessor dispatch (8-bit bus + handshake)",  150),
        ("IRQ controller (WAIT/RAISE, 4 channels)",      100),
        ("Misc glue (clock div, reset, status)",          100),
    ]
    total = sum(n for _, n in blocks)
    for name, n in blocks:
        if n > 0:
            print(f"    {name:52s} {n:5d}")
    print(f"    {'-'*52} {'-'*5}")
    print(f"    {'TOTAL':52s} {total:5d}")
    print(f"    {'Available':52s}  7680")
    pct = total / 7680 * 100
    spare = 7680 - total
    print(f"    {'Utilization':52s} {pct:.0f}%")
    print(f"    {'Spare LUTs':52s} {spare:5d}")
    print()
    print("    BRAM usage (128Kbit = 16KB in 32× EBR blocks):")
    bram = [
        ("Register file: 4 contexts × 16×32 = 256B",     1),
        ("Call stacks: 4 contexts × 8×32 = 128B",        1),
        ("Instruction prefetch buffer (64 words)",        1),
        ("W5100S TX/RX staging (2KB)",                    4),
        ("SD sector buffer (512B)",                       1),
        ("Coprocessor result buffer (256B)",              1),
    ]
    bram_total = sum(n for _, n in bram)
    for name, n in bram:
        print(f"      {name:50s} {n:2d} EBR")
    print(f"      {'-'*50} {'-'*2}")
    print(f"      {'TOTAL':50s} {bram_total:2d} / 32 EBR")
    return total <= 7680


# ═══════════════════════════════════════════════════════════════════════
# Power Budget
# ═══════════════════════════════════════════════════════════════════════

def power_budget_report():
    print("\n  Power Budget -- USB 5V / 500mA (2.5W budget)")
    print("  " + "-" * 60)
    rails = [
        ("iCE40HX8K core (1.2V, 30mA typical)",    0.036),
        ("iCE40HX8K I/O (3.3V, 40mA)",             0.132),
        ("SRAM (3.3V, 30mA active)",                0.099),
        ("W5100S (3.3V, 132mA active)",             0.436),
        ("SD card (3.3V, 50mA active)",             0.165),
        ("Config flash (3.3V, 5mA)",                0.017),
        ("LDO dropout loss (5V->3.3V, ~300mA)",     0.510),
        ("LEDs + misc",                             0.020),
    ]
    total = sum(w for _, w in rails)
    for name, w in rails:
        print(f"    {name:48s} {w:.3f}W")
    print(f"    {'-'*48} {'-'*5}")
    print(f"    {'TOTAL':48s} {total:.3f}W")
    print(f"    Budget: 2.5W (USB), margin: {2.5 - total:.2f}W")
    print()
    print("    With ECP5 coprocessor attached:")
    ecp5_power = 0.500  # ECP5-25F at moderate utilization
    combined = total + ecp5_power
    print(f"    {'+ ECP5-25F (1.1V core + I/O)':48s} {ecp5_power:.3f}W")
    print(f"    {'Combined total':48s} {combined:.3f}W")
    print(f"    Need: USB 5V/1A or PoE splitter ({combined:.1f}W < 5W budget)")


# ═══════════════════════════════════════════════════════════════════════
# Performance Model
# ═══════════════════════════════════════════════════════════════════════

def performance_report():
    print("\n  Performance Estimates")
    print("  " + "-" * 60)
    print()
    print("    Clock: 48MHz (iCE40 PLL from 12MHz crystal)")
    print()
    print("    Instruction throughput:")
    print("      Single-cycle ops (ADD/SUB/INC/BRANCH/JUMP):  48M IPS")
    print("      LOAD from SRAM (10ns + 1 cycle setup):        24M IPS")
    print("      LOAD from SD card (sector read ~1ms):          1K IPS")
    print("      PIPE (SRAM -> W5100S, per 2KB chunk):        750K/s")
    print("      MUL soft (shift-add):                          6M IPS")
    print("      DIV soft (restoring):                        1.5M IPS")
    print("      MUL via ECP5 (dispatch+execute+return):       12M IPS")
    print("      DSP via ECP5 (MATMUL 16×16):                  ~100K/s")
    print()
    print("    Query throughput (typical KV read: LOAD+PIPE):")
    print("      SRAM-cached card (hot path):          ~4M QPS")
    print("      SD card (cold, sequential):           ~500 QPS")
    print("      SD card (cold, random 512B sector):   ~200 QPS")
    print()
    print("    Network (W5100S limits):")
    print("      Max TCP throughput:                   ~15 Mbps (SPI bottleneck)")
    print("      Concurrent connections:              4 (hardware sockets)")
    print("      Practical HTTP responses/sec:         ~50K (small cards)")
    print()
    print("    With ECP5 coprocessor:")
    print("      MATMUL 16×16 (int16):                 1.4M MACs/cycle")
    print("      Vector dot product (128-dim):         ~2M/s")
    print("      SOFTMAX (64 elements):                ~500K/s")
    print()
    print("    Comparison to Pico tier (RP2354B):")
    print("      HX deterministic latency:     yes (no OS, no interrupts)")
    print("      HX parallel contexts:         4 (vs 1 on RP2354B)")
    print("      HX clock:                     48MHz (vs 150MHz but pipelined)")
    print("      HX effective throughput:      ~2-4× RP2354B for PicoScript")


# ═══════════════════════════════════════════════════════════════════════
# Coprocessor Interface Specification
# ═══════════════════════════════════════════════════════════════════════

def coprocessor_interface():
    print("\n  ECP5 Coprocessor Interface (20-pin header)")
    print("  " + "-" * 60)
    print("""
    Pin  Signal       Dir(HX)  Description
    ───  ──────────   ───────  ──────────────────────────────────
     1   VCC_3V3      OUT      Power to coprocessor (3.3V)
     2   GND          ---      Ground
     3   D0           BIDIR    Data bus bit 0
     4   D1           BIDIR    Data bus bit 1
     5   D2           BIDIR    Data bus bit 2
     6   D3           BIDIR    Data bus bit 3
     7   D4           BIDIR    Data bus bit 4
     8   D5           BIDIR    Data bus bit 5
     9   D6           BIDIR    Data bus bit 6
    10   D7           BIDIR    Data bus bit 7
    11   REQ#         OUT      HX requests DSP operation
    12   ACK#         IN       ECP5 acknowledges / result ready
    13   IRQ#         IN       ECP5 interrupt (async completion)
    14   OPCODE[0]    OUT      DSP sub-op select bit 0
    15   OPCODE[1]    OUT      DSP sub-op select bit 1
    16   OPCODE[2]    OUT      DSP sub-op select bit 2
    17   OPCODE[3]    OUT      DSP sub-op select bit 3
    18   RST#         OUT      Coprocessor reset
    19   VCC_3V3      OUT      Power (second pin for current)
    20   GND          ---      Ground (second pin)

    Protocol:
      1. HX puts DSP sub-op on OPCODE[3:0] + operand addr on D[7:0]
      2. HX asserts REQ#
      3. ECP5 reads operands from shared SRAM, executes DSP op
      4. ECP5 writes result to SRAM, asserts ACK#
      5. HX reads result address from D[7:0], deasserts REQ#

    Latency: 2-50 cycles depending on operation
    Bandwidth: 8 bits × 48MHz = 48 MB/s burst (descriptor only)
    Operand data: ECP5 reads/writes SRAM directly (shared bus, HX yields)
    """)


# ═══════════════════════════════════════════════════════════════════════
# KiCad generator helpers
# ═══════════════════════════════════════════════════════════════════════

def gen_sch(name, bom, title, comment):
    header = f"""(kicad_sch
  (version 20231120)
  (generator "picowal_hx_gen")
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


def gen_pcb(name, board_w, board_h, layers=2):
    header = f"""(kicad_pcb
  (version 20231014)
  (generator "picowal_hx_gen")
  (general (thickness 1.6))
  (paper "A4")
  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
{"    (1 " + '"In1.Cu" signal)\n    (2 "In2.Cu" signal)' if layers >= 4 else ""}
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

    # Base board schematic + PCB
    sch = gen_sch("hx_base", BASE_BOM,
                  "PicoWAL HX Base Board",
                  "iCE40HX8K + SRAM + W5100S + SD, 60x40mm")
    with open(os.path.join(OUTPUT_DIR, "hx_base.kicad_sch"), "w") as f:
        f.write(sch)

    pcb = gen_pcb("hx_base", 60, 40, layers=2)
    with open(os.path.join(OUTPUT_DIR, "hx_base.kicad_pcb"), "w") as f:
        f.write(pcb)
    print("[OK] hx_base.kicad_sch + .kicad_pcb (60x40mm, 2-layer)")

    # ECP5 coprocessor card
    sch = gen_sch("hx_coproc", COPROCESSOR_BOM,
                  "PicoWAL HX-AI Coprocessor",
                  "ECP5-25F DSP accelerator, 30x25mm")
    with open(os.path.join(OUTPUT_DIR, "hx_coproc.kicad_sch"), "w") as f:
        f.write(sch)

    pcb = gen_pcb("hx_coproc", 30, 25, layers=4)
    with open(os.path.join(OUTPUT_DIR, "hx_coproc.kicad_pcb"), "w") as f:
        f.write(pcb)
    print("[OK] hx_coproc.kicad_sch + .kicad_pcb (30x25mm, 4-layer)")

    # Summary
    print(f"\n{'='*65}")
    print(f" PicoWAL HX v{VERSION} -- Pure-Logic PicoScript Engine")
    print(f"{'='*65}")
    print()
    print(" Architecture: iCE40 runs PicoScript natively in LUTs.")
    print(" No CPU. No OS. No interrupts. Just combinatorial logic.")
    print()
    print(" Base Board (60x40mm, 2-layer, hand-solderable):")
    print("   U1: iCE40HX8K-TQ144   7680 LUTs, 206 I/O, 128Kbit BRAM")
    print("   U3: IS61WV25616BLL     512KB SRAM (10ns, card page cache)")
    print("   U4: W5100S             HW TCP/IP, 4 sockets, 100Mbps")
    print("   J3: Micro SD slot      Bulk card storage (WAL + data)")
    print("   J4: 2×10 header        Optional ECP5 coprocessor")
    print()
    print(" Optional Coprocessor (30x25mm, plugs onto J4):")
    print("   U1: LFE5U-25F          24K LUT, 56 DSP MACs")
    print("   Handles: MUL, DIV, MATMUL, SOFTMAX, all DSP sub-ops")
    print("   Interface: 8-bit dispatch bus + 4-bit opcode (48 MB/s)")
    print()
    print(" PicoScript Execution (13/16 opcodes native, single-cycle):")
    print("   NOOP  LOAD  SAVE  PIPE  ADD  SUB  INC")
    print("   JUMP  BRANCH  CALL  RETURN  WAIT  RAISE")
    print()
    print("   MUL/DIV: soft fallback (8/32 cycles) or ECP5 (1/3 cycles)")
    print("   DSP:     ECP5 only (returns ERR_NO_COPROC without card)")
    print()

    # BOM cost
    base_parts = sum(p["cost"] for p in BASE_BOM)
    base_pcb = 2.00  # 2-layer 60x40mm
    base_total = base_parts + base_pcb
    coproc_parts = sum(p["cost"] for p in COPROCESSOR_BOM)
    coproc_pcb = 5.00  # 4-layer BGA
    coproc_total = coproc_parts + coproc_pcb

    print(f" BOM Cost:")
    print(f"   Base board parts:                {chr(163)}{base_parts:.2f}")
    print(f"   Base PCB (2-layer, 60x40mm):     {chr(163)}{base_pcb:.2f}")
    print(f"   {'─'*40}")
    print(f"   HX total:                        {chr(163)}{base_total:.2f}")
    print()
    print(f"   Coprocessor parts:               {chr(163)}{coproc_parts:.2f}")
    print(f"   Coprocessor PCB (4-layer BGA):   {chr(163)}{coproc_pcb:.2f}")
    print(f"   {'─'*40}")
    print(f"   HX-AI total:                     {chr(163)}{base_total + coproc_total:.2f}")
    print()
    print(f"   SD card (32GB):                  {chr(163)}5.00")
    print(f"   {'─'*40}")
    print(f"   Complete HX system:              {chr(163)}{base_total + 5:.2f}")
    print(f"   Complete HX-AI system:           {chr(163)}{base_total + coproc_total + 5:.2f}")
    print()

    # Reports
    pin_budget_report()
    lut_budget_report()
    power_budget_report()
    performance_report()
    coprocessor_interface()

    print(f"\n{'='*65}")
    print(" Design philosophy:")
    print("   The iCE40 IS the PicoScript CPU. Every query lane is a")
    print("   hardwired state machine. No instruction cache misses.")
    print("   No branch prediction. No pipeline stalls. Just LUTs")
    print("   switching at 48MHz with zero-wait-state SRAM behind them.")
    print()
    print("   The ECP5 is a coprocessor, not a controller. It sits idle")
    print("   until the HX dispatches a DSP job. You only pay for AI")
    print("   acceleration if you need it.")
    print()
    print("   Total system cost for a working PicoScript query engine:")
    print(f"   {chr(163)}{base_total + 5:.2f} including storage. Hand-solderable.")
    print(f"{'='*65}")


if __name__ == "__main__":
    main()
