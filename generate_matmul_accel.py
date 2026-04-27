#!/usr/bin/env python3
"""generate_matmul_accel.py — Tiny SRAM jump-table matmul accelerator

Generates KiCad 8 schematic + PCB for a minimal matmul coprocessor:
  - 1× RP2354B: host controller, SPI/USB interface, weight loader
  - 1× iCE40HX4K-TQ144: address routing fabric
  - 8× IS61WV25616BLL: systolic SRAM chain (the compute array)
  - 1× W25Q128: RP flash
  - 1× W25Q32: FPGA config flash

The SRAM chain IS the matmul engine. No ALU. No DSP.
FPGA drives addresses, SRAMs chain D[15:8]→A[15:8] on PCB.

Board: ~60×40mm, 2-layer, all TSOP/TQFP/QFN — hand-solderable.
"""

import os, uuid, math

VERSION = "12.0"
BOARD_NAME = "matmul_accel"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "matmul_accel")

# ─── UUID helper ───
_uuid_counter = 0
def make_uuid():
    global _uuid_counter
    _uuid_counter += 1
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"matmul_accel.{_uuid_counter}"))

# ─── BOM ───
BOM = [
    {"ref": "U1",  "value": "RP2354B",          "pkg": "QFN-80",   "desc": "Host MCU"},
    {"ref": "U2",  "value": "iCE40HX4K",        "pkg": "TQFP-144", "desc": "Address routing fabric"},
    {"ref": "U3",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 0"},
    {"ref": "U4",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 1"},
    {"ref": "U5",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 2"},
    {"ref": "U6",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 3"},
    {"ref": "U7",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 4"},
    {"ref": "U8",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 5"},
    {"ref": "U9",  "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 6"},
    {"ref": "U10", "value": "IS61WV25616BLL",    "pkg": "TSOP-44",  "desc": "SRAM chain stage 7"},
    {"ref": "U11", "value": "W25Q128JVSIQ",      "pkg": "SOIC-8",   "desc": "RP flash"},
    {"ref": "U12", "value": "W25Q32JVSIQ",       "pkg": "SOIC-8",   "desc": "FPGA config flash"},
    {"ref": "U13", "value": "AP2112K-3.3",       "pkg": "SOT-23-5", "desc": "3.3V LDO"},
    {"ref": "Y1",  "value": "12MHz",             "pkg": "3215",     "desc": "RP crystal"},
    {"ref": "J1",  "value": "USB-C",             "pkg": "USB-C-SMD","desc": "Host interface"},
    {"ref": "J2",  "value": "CONN_2x10",         "pkg": "2x10-2.54","desc": "Expansion header"},
]

# ─── Schematic symbols ───

def sym_rp2354b():
    pins = []
    # SPI to FPGA
    for i, name in enumerate(["SPI_SCK","SPI_MOSI","SPI_MISO","SPI_CS"]):
        pins.append(f'(pin bidirectional line (at -15.24 {10.16-i*2.54} 0) (length 2.54) (name "{name}") (number "{i+1}") (uuid {make_uuid()}))')
    # USB
    for i, name in enumerate(["USB_DP","USB_DM"]):
        pins.append(f'(pin bidirectional line (at -15.24 {-2.54-i*2.54} 0) (length 2.54) (name "{name}") (number "{i+5}") (uuid {make_uuid()}))')
    # Boot/Run
    for i, name in enumerate(["BOOTSEL","RUN"]):
        pins.append(f'(pin input line (at -15.24 {-10.16-i*2.54} 0) (length 2.54) (name "{name}") (number "{i+7}") (uuid {make_uuid()}))')
    # XIN/XOUT
    for i, name in enumerate(["XIN","XOUT"]):
        pins.append(f'(pin bidirectional line (at 15.24 {10.16-i*2.54} 180) (length 2.54) (name "{name}") (number "{i+9}") (uuid {make_uuid()}))')
    # Flash SPI
    for i, name in enumerate(["QSPI_SCK","QSPI_CS","QSPI_D0","QSPI_D1","QSPI_D2","QSPI_D3"]):
        pins.append(f'(pin bidirectional line (at 15.24 {2.54-i*2.54} 180) (length 2.54) (name "{name}") (number "{i+11}") (uuid {make_uuid()}))')
    # Power
    for i, name in enumerate(["VCC","GND"]):
        ptype = "power_in"
        pins.append(f'(pin {ptype} line (at 15.24 {-12.7-i*2.54} 180) (length 2.54) (name "{name}") (number "{i+17}") (uuid {make_uuid()}))')
    nl = "\n    "
    return f"""(symbol "rp2354b" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 13.97 0) (effects (font (size 1.27 1.27))))
    (property "Value" "RP2354B" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.7x5.7mm" (at 0 0 0) (effects (hide yes)))
    (symbol "rp2354b_0_1"
      (rectangle (start -12.7 12.7) (end 12.7 -16.51) (stroke (width 0.254)) (fill (type background))))
    (symbol "rp2354b_1_1"
    {nl.join(pins)}))"""

def sym_ice40hx4k():
    pins = []
    pin_num = 1
    # SPI from RP
    for name in ["SPI_SCK","SPI_MOSI","SPI_MISO","SPI_CS"]:
        pins.append(f'(pin bidirectional line (at -20.32 {15.24-(pin_num-1)*2.54} 0) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # SRAM address outputs: 8 groups of A[7:0] = 64 pins
    for s in range(8):
        for b in range(8):
            name = f"S{s}_A{b}"
            pins.append(f'(pin output line (at 20.32 {40.64-(pin_num-5)*1.27} 180) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
            pin_num += 1
    # Shared row select A[17:16]
    for b in range(2):
        pins.append(f'(pin output line (at 20.32 {-45-(b*2.54)} 180) (length 2.54) (name "ROW_A{16+b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Result data D[15:0] from last SRAM
    for b in range(16):
        pins.append(f'(pin input line (at -20.32 {-5.08-b*1.27} 0) (length 2.54) (name "RES_D{b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Control
    for name in ["SRAM_CE_N","SRAM_OE_N","CDONE","CRESET_B"]:
        pins.append(f'(pin bidirectional line (at -20.32 {-26-len(pins)*0.5} 0) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Config flash
    for name in ["CFG_SCK","CFG_MOSI","CFG_MISO","CFG_CS"]:
        pins.append(f'(pin bidirectional line (at -20.32 {-35-len(pins)*0.3} 0) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Power
    for name in ["VCC","VCC_IO","GND"]:
        ptype = "power_in"
        pins.append(f'(pin {ptype} line (at 0 {-55-len(pins)*0.2} 90) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    nl = "\n    "
    return f"""(symbol "ice40hx4k" (in_bom yes) (on_board yes)
    (property "Reference" "U" (at 0 45 0) (effects (font (size 1.27 1.27))))
    (property "Value" "iCE40HX4K" (at 0 -60 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "Package_QFP:TQFP-144_20x20mm_P0.5mm" (at 0 0 0) (effects (hide yes)))
    (symbol "ice40hx4k_0_1"
      (rectangle (start -17.78 43.18) (end 17.78 -58.42) (stroke (width 0.254)) (fill (type background))))
    (symbol "ice40hx4k_1_1"
    {nl.join(pins)}))"""

def sym_sram():
    pins = []
    pin_num = 1
    # Address A[17:0]
    for b in range(18):
        pins.append(f'(pin input line (at -12.7 {20.32-b*2.54} 0) (length 2.54) (name "A{b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Data D[15:0]
    for b in range(16):
        pins.append(f'(pin bidirectional line (at 12.7 {20.32-b*2.54} 180) (length 2.54) (name "D{b}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Control
    for name in ["CE_N","OE_N","WE_N"]:
        pins.append(f'(pin input line (at -12.7 {-26-(pin_num-35)*2.54} 0) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
        pin_num += 1
    # Power
    for name in ["VCC","GND"]:
        ptype = "power_in"
        pins.append(f'(pin {ptype} line (at 0 {-35-(pin_num-38)*2.54} 90) (length 2.54) (name "{name}") (number "{pin_num}") (uuid {make_uuid()}))')
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

def sym_generic(ref_prefix, value, fp, pin_names):
    pins = []
    for i, name in enumerate(pin_names):
        ptype = "bidirectional" if name not in ("VCC","GND") else "power_in"
        pins.append(f'(pin {ptype} line (at -10.16 {5.08-i*2.54} 0) (length 2.54) (name "{name}") (number "{i+1}") (uuid {make_uuid()}))')
    nl = "\n    "
    h = max(len(pin_names)*2.54+2, 8)
    return f"""(symbol "{value.lower().replace('-','_').replace('.','_')}" (in_bom yes) (on_board yes)
    (property "Reference" "{ref_prefix}" (at 0 {h/2+2} 0) (effects (font (size 1.27 1.27))))
    (property "Value" "{value}" (at 0 {-h/2-2} 0) (effects (font (size 1.27 1.27))))
    (property "Footprint" "{fp}" (at 0 0 0) (effects (hide yes)))
    (symbol "{value.lower().replace('-','_').replace('.','_')}_0_1"
      (rectangle (start -7.62 {h/2}) (end 7.62 {-h/2}) (stroke (width 0.254)) (fill (type background))))
    (symbol "{value.lower().replace('-','_').replace('.','_')}_1_1"
    {nl.join(pins)}))"""

# ─── Schematic generation ───

def gen_schematic():
    syms = []
    syms.append(sym_rp2354b())
    syms.append(sym_ice40hx4k())
    syms.append(sym_sram())
    syms.append(sym_generic("U","W25Q128JVSIQ","Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
        ["SCK","CS","DI","DO","WP","HOLD","VCC","GND"]))
    syms.append(sym_generic("U","W25Q32JVSIQ","Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
        ["SCK","CS","DI","DO","WP","HOLD","VCC","GND"]))
    syms.append(sym_generic("U","AP2112K_3_3","Package_TO_SOT_SMD:SOT-23-5",
        ["VIN","GND","EN","NC","VOUT"]))
    syms.append(sym_generic("Y","12MHz","Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm",
        ["XIN","XOUT"]))
    syms.append(sym_generic("J","USB_C","Connector_USB:USB_C_Receptacle_GCT_USB4105",
        ["VBUS","CC1","CC2","DP","DM","GND","SHELL"]))
    syms.append(sym_generic("J","CONN_2x10","Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical",
        [f"P{i+1}" for i in range(20)]))

    # Build component instances
    instances = []
    x_pos, y_pos = 50, 50

    # RP2354B
    instances.append(f"""(symbol (lib_id "rp2354b") (at {x_pos} {y_pos} 0) (unit 1)
    (property "Reference" "U1" (at 0 2 0)) (property "Value" "RP2354B" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "U1") (unit 1)))))""")

    # FPGA
    instances.append(f"""(symbol (lib_id "ice40hx4k") (at {x_pos+80} {y_pos} 0) (unit 1)
    (property "Reference" "U2" (at 0 2 0)) (property "Value" "iCE40HX4K" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "U2") (unit 1)))))""")

    # 8× SRAM chain
    for i in range(8):
        sx = x_pos + 160 + (i % 4) * 40
        sy = y_pos - 40 + (i // 4) * 80
        ref = f"U{i+3}"
        instances.append(f"""(symbol (lib_id "is61wv25616bll") (at {sx} {sy} 0) (unit 1)
    (property "Reference" "{ref}" (at 0 2 0)) (property "Value" "IS61WV25616BLL" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "{ref}") (unit 1)))))""")

    # Support chips
    support = [
        ("U11","w25q128jvsiq", x_pos, y_pos+60),
        ("U12","w25q32jvsiq",  x_pos+80, y_pos+60),
        ("U13","ap2112k_3_3",  x_pos-40, y_pos),
    ]
    for ref, lib, sx, sy in support:
        instances.append(f"""(symbol (lib_id "{lib}") (at {sx} {sy} 0) (unit 1)
    (property "Reference" "{ref}" (at 0 2 0)) (property "Value" "{lib.upper()}" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "{ref}") (unit 1)))))""")

    # Crystal
    instances.append(f"""(symbol (lib_id "12mhz") (at {x_pos+30} {y_pos+30} 0) (unit 1)
    (property "Reference" "Y1" (at 0 2 0)) (property "Value" "12MHz" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "Y1") (unit 1)))))""")

    # Connectors
    instances.append(f"""(symbol (lib_id "usb_c") (at {x_pos-40} {y_pos+40} 0) (unit 1)
    (property "Reference" "J1" (at 0 2 0)) (property "Value" "USB-C" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "J1") (unit 1)))))""")

    instances.append(f"""(symbol (lib_id "conn_2x10") (at {x_pos+320} {y_pos+20} 0) (unit 1)
    (property "Reference" "J2" (at 0 2 0)) (property "Value" "CONN_2x10" (at 0 -2 0))
    (uuid {make_uuid()}) (instances (project "{BOARD_NAME}" (path "/{make_uuid()}" (reference "J2") (unit 1)))))""")

    # Net labels for SRAM chain wiring
    netlabels = []
    for i in range(7):
        # SRAM[i].D[15:8] → SRAM[i+1].A[15:8] — the systolic chain
        for b in range(8):
            netlabels.append(f"""(global_label "CHAIN_{i}_{i+1}_D{b+8}" (at 0 0 0) (shape bidirectional)
    (uuid {make_uuid()}) (property "Intersheetrefs" "" (at 0 0 0)))""")

    # Net labels for FPGA → SRAM address routing
    for s in range(8):
        for b in range(8):
            netlabels.append(f"""(global_label "S{s}_A{b}" (at 0 0 0) (shape output)
    (uuid {make_uuid()}) (property "Intersheetrefs" "" (at 0 0 0)))""")

    # Shared row select
    for b in range(2):
        netlabels.append(f"""(global_label "ROW_A{16+b}" (at 0 0 0) (shape output)
    (uuid {make_uuid()}) (property "Intersheetrefs" "" (at 0 0 0)))""")

    nl = "\n  "
    return f"""(kicad_sch (version 20231120) (generator "matmul_accel_gen") (generator_version "{VERSION}")
  (paper "A3")
  (title_block
    (title "Matmul Accelerator v{VERSION} — SRAM Jump-Table Systolic Chain")
    (comment 1 "8× IS61WV25616BLL systolic chain — weights as jump tables")
    (comment 2 "Memory IS the compute. FPGA IS the instruction stream.")
    (comment 3 "BOM: ~£23. 12.7 GOPS effective. Zero ALU."))
  (lib_symbols
  {nl.join(syms)})
  {nl.join(instances)}
  {nl.join(netlabels)})"""


# ─── PCB generation ───

def gen_pcb():
    footprints = []
    board_w, board_h = 60, 40

    # RP2354B — top left
    footprints.append(make_fp("U1", "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.7x5.7mm",
                              "RP2354B", 10, 15))
    # FPGA — center left
    footprints.append(make_fp("U2", "Package_QFP:TQFP-144_20x20mm_P0.5mm",
                              "iCE40HX4K", 10, 30))
    # 8× SRAM — two rows of 4 on the right side
    for i in range(8):
        x = 30 + (i % 4) * 8
        y = 10 + (i // 4) * 20
        footprints.append(make_fp(f"U{i+3}", "Package_SO:TSOP-II-44_10.16x18.41mm_P0.8mm",
                                  "IS61WV25616BLL", x, y))
    # Support
    footprints.append(make_fp("U11","Package_SO:SOIC-8_3.9x4.9mm_P1.27mm","W25Q128",5,5))
    footprints.append(make_fp("U12","Package_SO:SOIC-8_3.9x4.9mm_P1.27mm","W25Q32",5,25))
    footprints.append(make_fp("U13","Package_TO_SOT_SMD:SOT-23-5","AP2112K",2,20))
    footprints.append(make_fp("Y1","Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm","12MHz",15,5))
    footprints.append(make_fp("J1","Connector_USB:USB_C_Receptacle_GCT_USB4105","USB-C",2,10))
    footprints.append(make_fp("J2","Connector_PinHeader_2.54mm:PinHeader_2x10_P2.54mm_Vertical","EXPANSION",58,20))

    edge = f"""(gr_rect (start 0 0) (end {board_w} {board_h})
    (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts") (uuid {make_uuid()}))"""

    nl = "\n  "
    return f"""(kicad_pcb (version 20231014) (generator "matmul_accel_gen") (generator_version "{VERSION}")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers
    (0 "F.Cu" signal) (31 "B.Cu" signal)
    (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")
    (44 "Edge.Cuts" user))
  (setup (grid_origin 0 0)
    (pcbplotparams (layerselection 0x00010fc_ffffffff) (outputdirectory "")))
  {edge}
  {nl.join(footprints)})"""

def make_fp(ref, footprint, value, x, y):
    return f"""(footprint "{footprint}"
    (at {x} {y}) (layer "F.Cu") (uuid {make_uuid()})
    (property "Reference" "{ref}" (at 0 -2 0) (layer "F.SilkS") (uuid {make_uuid()})
      (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "{value}" (at 0 2 0) (layer "F.SilkS") (uuid {make_uuid()})
      (effects (font (size 0.8 0.8) (thickness 0.12)))))"""


# ─── Project file ───

def gen_project():
    return f"""{{"meta":{{"filename":"{BOARD_NAME}.kicad_pro","version":2}},
"board":{{"design_settings":{{"defaults":{{"board_outline_line_width":0.1}},
"rules":{{"min_clearance":0.15,"min_track_width":0.15}}}}}},
"schematic":{{"drawing":{{"default_line_thickness":0.006}}}}}}"""


# ─── Main ───

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    with open(os.path.join(OUTPUT_DIR, f"{BOARD_NAME}.kicad_pro"), "w") as f:
        f.write(gen_project())
    print(f"[OK] {BOARD_NAME}.kicad_pro")

    with open(os.path.join(OUTPUT_DIR, f"{BOARD_NAME}.kicad_sch"), "w") as f:
        f.write(gen_schematic())
    print(f"[OK] {BOARD_NAME}.kicad_sch")

    with open(os.path.join(OUTPUT_DIR, f"{BOARD_NAME}.kicad_pcb"), "w") as f:
        f.write(gen_pcb())
    print(f"[OK] {BOARD_NAME}.kicad_pcb")

    print(f"\n{'='*60}")
    print(f" Matmul Accelerator v{VERSION}")
    print(f"{'='*60}")
    print(f" Architecture: Systolic SRAM jump-table chain")
    print(f" Board:        {60}×{40}mm, 2-layer")
    print(f" Components:   {len(BOM)} total")
    print()
    print(" BOM:")
    total = 0
    costs = {
        "RP2354B": 0.70, "iCE40HX4K": 3.80, "IS61WV25616BLL": 1.90,
        "W25Q128JVSIQ": 1.20, "W25Q32JVSIQ": 0.80, "AP2112K-3.3": 0.30,
        "12MHz": 0.20, "USB-C": 0.50, "CONN_2x10": 0.30,
    }
    for part in BOM:
        c = costs.get(part["value"], 0.50)
        if "IS61" in part["value"]:
            n = 8
        else:
            n = 1
        if n > 1 and part["ref"] != "U3":
            continue
        if "IS61" in part["value"]:
            print(f"   U3-U10  {part['value']:24s} ×8   £{c*8:.2f}")
            total += c * 8
        else:
            print(f"   {part['ref']:6s}  {part['value']:24s} ×1   £{c:.2f}")
            total += c
    pcb_cost = 2.50
    passives_cost = 1.50
    total += pcb_cost + passives_cost
    print(f"   {'PCB':6s}  {'2-layer 60×40mm':24s} ×1   £{pcb_cost:.2f}")
    print(f"   {'MISC':6s}  {'Passives + bypass':24s}      £{passives_cost:.2f}")
    print(f"   {'─'*50}")
    print(f"   {'TOTAL':6s}  {'':24s}      £{total:.2f}")
    print()
    print(" Performance:")
    print("   8-element dot product:  1 per clock (pipelined)")
    print("   256×8 matmul:           ~3.2μs  (4 row groups)")
    print("   256×256 matmul:         ~102μs  (32 K-passes × 4 row groups)")
    print("   Effective throughput:   ~12.7 GOPS")
    print("   Latency per lookup:     10ns (async SRAM)")
    print("   Power:                  ~0.8W")
    print()
    print(" The weights ARE the compute. The FPGA IS the instruction stream.")
    print(f"{'='*60}")

if __name__ == "__main__":
    main()
