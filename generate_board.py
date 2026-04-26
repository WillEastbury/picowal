#!/usr/bin/env python3
"""
KiCad 8 project generator for RP2354 110-Node Network Compute Fabric v1.0.

Architecture (SPI Star Topology via FPGA):
  - 110x RP2354B (QFN-80, 48 GPIO) — 220 cores
  - All workers are SPI slaves on a shared bus, selected by 74HC595 shift register CS
  - Per node: 520KB SRAM + 2MB onboard flash (CS0) + 8MB PSRAM (CS1)
  - 110x LY68L6400 8MB QSPI PSRAM (SOP-8)
  - 1x iCE40HX4K-TQ144 FPGA (SPI star controller + SD RAID + cache)
  - 1x W5500 QFN-48 Ethernet chip (on-board, FPGA-attached)
  - 1x W25Q32 SPI flash (FPGA bitstream)
  - 2x APS6404L 8MB QSPI PSRAM (FPGA cache)
  - 5x MicroSD card slots (RAID-0 via FPGA)
  - 1x USB-C receptacle (UFP/sink)
  - 1x RJ45 MagJack with PoE center taps
  - 1x Ag9905 PoE PD controller
  - 14x PCF8574/PCF8574A I2C GPIO expander (CS mux / bootstrap)
  - 14x 74HC244 octal buffer (clock fanout)
  - 14x 74HC595 shift register (CS line cascade)
  - 4x TPS54560 buck converter — 12V->3.3V/5A each (4-zone rail)
  - 1x Phoenix screw terminal — 12V DC input (or PoE)
  - 4x status LEDs (PWR green, SYS blue, ERR red, NET yellow)
  - 1x Grove I2C connector
  - 4x M3 mounting holes
  - SWD test pads per node
  - 200mm x 200mm, 4-layer PCB

Generates:
  cluster_board.kicad_pro
  cluster_board.kicad_sch       (top-level hierarchical)
  worker_node.kicad_sch         (RP2354B worker + PSRAM, x110)
  power_supply.kicad_sch        (4x TPS54560 + PoE PD)
  bootstrap.kicad_sch           (14x PCF8574/PCF8574A expanders)
  clock_tree.kicad_sch          (oscillator + 14x 74HC244 buffers)
  fpga_subsystem.kicad_sch      (iCE40HX4K + W5500 + SD RAID + cache + 74HC595 CS)
  cluster_board.kicad_pcb       (PCB outline + placement)
"""

import json, os

OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

_uuid_counter = 0
def nuuid():
    global _uuid_counter
    _uuid_counter += 1
    return f"{_uuid_counter:08x}-0000-4000-8000-{_uuid_counter:012x}"

NUM_WORKERS = 110
NUM_CHAINS = 0
BOARD_W = 200
BOARD_H = 200

# ── KiCad project file ─────────────────────────────────────────────

def gen_project():
    proj = {
        "meta": {"filename": "cluster_board.kicad_pro", "version": 1},
        "board": {
            "3dviewports": [],
            "design_settings": {
                "defaults": {"board_outline_line_width": 0.1},
                "rules": {"min_clearance": 0.15, "min_track_width": 0.15,
                           "min_via_diameter": 0.6, "min_via_annular_width": 0.15}
            },
            "layer_presets": []
        },
        "schematic": {"drawing": {"default_line_thickness": 6.0}, "meta": {"version": 1}},
        "text_variables": {}
    }
    path = os.path.join(OUTPUT_DIR, "cluster_board.kicad_pro")
    with open(path, "w") as f:
        json.dump(proj, f, indent=2)
    print(f"  wrote {path}")

# ── Schematic helpers ───────────────────────────────────────────────

def sch_header(paper="A1", title="", rev="1.0"):
    return f"""(kicad_sch
  (version 20231120)
  (generator "copilot_cluster_gen")
  (generator_version "2.0")
  (uuid "{nuuid()}")
  (paper "{paper}")
  (title_block
    (title "{title}")
    (rev "{rev}")
    (company "RP2354 Cluster Tile")
  )
"""

def sch_footer():
    return ")\n"

def place_symbol(lib_id, ref, value, x, y, angle=0, unit=1, extra_props=None):
    u = nuuid()
    props = ""
    if extra_props:
        for k, v in extra_props.items():
            props += f'\n    (property "{k}" "{v}" (at {x} {y-5.08} 0) (effects (font (size 1.27 1.27)) hide))'
    return f"""  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} {angle}) (unit {unit})
    (in_bom yes) (on_board yes) (dnp no)
    (uuid "{u}")
    (property "Reference" "{ref}" (at {x:.2f} {y+2.54:.2f} 0) (effects (font (size 1.27 1.27))))
    (property "Value" "{value}" (at {x:.2f} {y-2.54:.2f} 0) (effects (font (size 1.27 1.27)))){props}
  )
"""

def place_wire(x1, y1, x2, y2):
    return f'  (wire (pts (xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f})) (uuid "{nuuid()}"))\n'

def place_label(name, x, y, angle=0):
    return f'  (label "{name}" (at {x:.2f} {y:.2f} {angle}) (uuid "{nuuid()}") (effects (font (size 1.27 1.27))))\n'

def place_global_label(name, x, y, angle=0, shape="bidirectional"):
    return f'  (global_label "{name}" (shape {shape}) (at {x:.2f} {y:.2f} {angle}) (uuid "{nuuid()}") (effects (font (size 1.27 1.27))))\n'

def place_hier_label(name, x, y, angle=0, shape="bidirectional"):
    return f'  (hierarchical_label "{name}" (shape {shape}) (at {x:.2f} {y:.2f} {angle}) (uuid "{nuuid()}") (effects (font (size 1.27 1.27))))\n'

def place_hier_sheet(name, filename, x, y, w, h, pins):
    u = nuuid()
    pin_str = ""
    for pname, pshape, px, py, pangle in pins:
        pin_str += f'    (pin "{pname}" {pshape} (at {x+px:.2f} {y+py:.2f} {pangle}) (uuid "{nuuid()}")\n      (effects (font (size 1.27 1.27)))\n    )\n'
    return f"""  (sheet (at {x:.2f} {y:.2f}) (size {w:.2f} {h:.2f})
    (uuid "{u}")
    (property "Sheetname" "{name}" (at {x:.2f} {y-1.27:.2f} 0) (effects (font (size 1.27 1.27))))
    (property "Sheetfile" "{filename}" (at {x:.2f} {y+h+1.27:.2f} 0) (effects (font (size 1.27 1.27))))
{pin_str}  )
"""

def place_text(text, x, y, size=2.54):
    return f'  (text "{text}" (at {x:.2f} {y:.2f} 0) (uuid "{nuuid()}") (effects (font (size {size} {size}))))\n'

# ── Library Symbols ─────────────────────────────────────────────────

def lib_symbol_resistor():
    return """    (symbol "Device:R" (pin_numbers hide) (pin_names (offset 0)) (in_bom yes) (on_board yes)
      (property "Reference" "R" (at 2.032 0 90) (effects (font (size 1.27 1.27))))
      (property "Value" "R" (at -2.032 0 90) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Resistor_SMD:R_0402_1005Metric" (at -1.778 0 90) (effects (font (size 1.27 1.27)) hide))
      (symbol "R_0_1"
        (rectangle (start -1.016 2.286) (end 1.016 -2.286) (stroke (width 0.254) (type default)) (fill (type none)))
      )
      (symbol "R_1_1"
        (pin passive line (at 0 -2.54 90) (length 0) (name "~" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 0 2.54 270) (length 0) (name "~" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_cap():
    return """    (symbol "Device:C" (pin_numbers hide) (pin_names (offset 0.254)) (in_bom yes) (on_board yes)
      (property "Reference" "C" (at 1.524 0 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Value" "C" (at 1.524 -2.54 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Footprint" "Capacitor_SMD:C_0402_1005Metric" (at 0.9652 -3.81 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "C_0_1"
        (polyline (pts (xy -1.524 -0.508) (xy 1.524 -0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
        (polyline (pts (xy -1.524 0.508) (xy 1.524 0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
      )
      (symbol "C_1_1"
        (pin passive line (at 0 2.54 270) (length 2.032) (name "~" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 0 -2.54 90) (length 2.032) (name "~" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_led():
    return """    (symbol "Device:LED" (pin_numbers hide) (pin_names (offset 1.016) hide) (in_bom yes) (on_board yes)
      (property "Reference" "D" (at 0 2.54 0) (effects (font (size 1.27 1.27))))
      (property "Value" "LED" (at 0 -2.54 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "LED_SMD:LED_0603_1608Metric" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "LED_0_1"
        (polyline (pts (xy 1.27 -1.27) (xy 1.27 1.27) (xy -1.27 0) (xy 1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type outline)))
      )
      (symbol "LED_1_1"
        (pin passive line (at -3.81 0 0) (length 2.54) (name "A" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 3.81 0 180) (length 2.54) (name "K" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_rp2354b():
    """RP2354B QFN-80 — 48 GPIO, 2MB onboard flash. All nodes use this."""
    pins = []
    for i in range(48):
        y = 58.42 - i * 2.54
        pins.append(f'        (pin bidirectional line (at -20.32 {y:.2f} 0) (length 2.54) (name "GPIO{i}" (effects (font (size 1.016 1.016)))) (number "{i+1}" (effects (font (size 1.016 1.016)))))')
    pwr = [("DVDD", "49", "power_in"), ("VREG_VIN", "50", "power_in"),
           ("VREG_VOUT", "51", "power_out"), ("USB_DP", "52", "bidirectional"),
           ("USB_DM", "53", "bidirectional"), ("XIN", "54", "input"),
           ("XOUT", "55", "output"), ("TESTEN", "56", "input"),
           ("SWCLK", "57", "input"), ("SWDIO", "58", "bidirectional"),
           ("RUN", "59", "input"), ("ADC_AVDD", "60", "power_in")]
    for i, (name, num, ptype) in enumerate(pwr):
        y = 20.32 - i * 2.54
        pins.append(f'        (pin {ptype} line (at 20.32 {y:.2f} 180) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    for i, num in enumerate(["61", "62", "63", "64", "65"]):
        pins.append(f'        (pin power_in line (at 20.32 {-7.62 - i*2.54:.2f} 180) (length 2.54) (name "GND" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    pins_str = "\n".join(pins)
    return f"""    (symbol "MCU_RaspberryPi:RP2354B" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 63.5 0) (effects (font (size 1.27 1.27))))
      (property "Value" "RP2354B" (at 0 -22.86 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "RP2354B_0_1"
        (rectangle (start -17.78 60.96) (end 17.78 -20.32) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "RP2354B_1_1"
{pins_str}
      )
    )"""

def lib_symbol_tps54560():
    return """    (symbol "Regulator_Switching:TPS54560" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "TPS54560" (at 0 -12.7 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:HSOP-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x3.1mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "TPS54560_0_1"
        (rectangle (start -10.16 10.16) (end 10.16 -10.16) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "TPS54560_1_1"
        (pin power_in line (at -12.7 7.62 0) (length 2.54) (name "VIN" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 2.54 0) (length 2.54) (name "EN" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -2.54 0) (length 2.54) (name "RT/CLK" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -7.62 0) (length 2.54) (name "FB" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 12.7 -7.62 180) (length 2.54) (name "COMP" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -2.54 180) (length 2.54) (name "BOOT" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 2.54 180) (length 2.54) (name "PH" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 12.7 7.62 180) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_barrel_jack():
    """High-current screw terminal for 12V input."""
    return """    (symbol "Connector:Screw_Terminal_01x02" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Value" "12V_DC" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-2-5.0-H_1x02_P5.00mm_Horizontal" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Screw_Terminal_01x02_0_1"
        (rectangle (start -5.08 2.54) (end 5.08 -2.54) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Screw_Terminal_01x02_1_1"
        (pin power_out line (at 7.62 0 180) (length 2.54) (name "+12V" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 7.62 -2.54 180) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_inductor():
    return """    (symbol "Device:L" (pin_numbers hide) (pin_names (offset 1.016) hide) (in_bom yes) (on_board yes)
      (property "Reference" "L" (at -1.016 0 90) (effects (font (size 1.27 1.27))))
      (property "Value" "L" (at 1.016 0 90) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Inductor_SMD:L_1210_3225Metric" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "L_0_1"
        (arc (start 0 -2.54) (mid 0.6323 -1.905) (end 0 -1.27) (stroke (width 0) (type default)) (fill (type none)))
        (arc (start 0 -1.27) (mid 0.6323 -0.635) (end 0 0) (stroke (width 0) (type default)) (fill (type none)))
        (arc (start 0 0) (mid 0.6323 0.635) (end 0 1.27) (stroke (width 0) (type default)) (fill (type none)))
        (arc (start 0 1.27) (mid 0.6323 1.905) (end 0 2.54) (stroke (width 0) (type default)) (fill (type none)))
      )
      (symbol "L_1_1"
        (pin passive line (at 0 2.54 270) (length 0) (name "1" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 0 -2.54 90) (length 0) (name "2" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_pcf8574():
    """PCF8574 I2C GPIO expander — SOIC-16, 8-bit output."""
    return """    (symbol "Interface_I2C:PCF8574" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 15.24 0) (effects (font (size 1.27 1.27))))
      (property "Value" "PCF8574" (at 0 -15.24 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "PCF8574_0_1"
        (rectangle (start -10.16 12.7) (end 10.16 -12.7) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "PCF8574_1_1"
        (pin input line (at -12.7 10.16 0) (length 2.54) (name "A0" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 7.62 0) (length 2.54) (name "A1" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 5.08 0) (length 2.54) (name "A2" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -12.7 0 0) (length 2.54) (name "SDA" (effects (font (size 1.27 1.27)))) (number "15" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -2.54 0) (length 2.54) (name "SCL" (effects (font (size 1.27 1.27)))) (number "14" (effects (font (size 1.27 1.27)))))
        (pin output line (at -12.7 -7.62 0) (length 2.54) (name "INT" (effects (font (size 1.27 1.27)))) (number "13" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 10.16 180) (length 2.54) (name "P0" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 7.62 180) (length 2.54) (name "P1" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 5.08 180) (length 2.54) (name "P2" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 2.54 180) (length 2.54) (name "P3" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 0 180) (length 2.54) (name "P4" (effects (font (size 1.27 1.27)))) (number "9" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 -2.54 180) (length 2.54) (name "P5" (effects (font (size 1.27 1.27)))) (number "10" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 -5.08 180) (length 2.54) (name "P6" (effects (font (size 1.27 1.27)))) (number "11" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 12.7 -7.62 180) (length 2.54) (name "P7" (effects (font (size 1.27 1.27)))) (number "12" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 15.24 270) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "16" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 -15.24 90) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_74hc244():
    """74HC244 octal buffer — TSSOP-20, clock fanout."""
    return """    (symbol "74xx:74HC244" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 17.78 0) (effects (font (size 1.27 1.27))))
      (property "Value" "74HC244" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "74HC244_0_1"
        (rectangle (start -10.16 15.24) (end 10.16 -15.24) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "74HC244_1_1"
        (pin input line (at -12.7 12.7 0) (length 2.54) (name "1A1" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 10.16 0) (length 2.54) (name "1A2" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 7.62 0) (length 2.54) (name "1A3" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 5.08 0) (length 2.54) (name "1A4" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 0 0) (length 2.54) (name "2A1" (effects (font (size 1.27 1.27)))) (number "11" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -2.54 0) (length 2.54) (name "2A2" (effects (font (size 1.27 1.27)))) (number "13" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -5.08 0) (length 2.54) (name "2A3" (effects (font (size 1.27 1.27)))) (number "15" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -7.62 0) (length 2.54) (name "2A4" (effects (font (size 1.27 1.27)))) (number "17" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 12.7 180) (length 2.54) (name "1Y1" (effects (font (size 1.27 1.27)))) (number "18" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 10.16 180) (length 2.54) (name "1Y2" (effects (font (size 1.27 1.27)))) (number "16" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 7.62 180) (length 2.54) (name "1Y3" (effects (font (size 1.27 1.27)))) (number "14" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 5.08 180) (length 2.54) (name "1Y4" (effects (font (size 1.27 1.27)))) (number "12" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 0 180) (length 2.54) (name "2Y1" (effects (font (size 1.27 1.27)))) (number "9" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -2.54 180) (length 2.54) (name "2Y2" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -5.08 180) (length 2.54) (name "2Y3" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -7.62 180) (length 2.54) (name "2Y4" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -12.7 0) (length 2.54) (name "1OE" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 -15.24 0) (length 2.54) (name "2OE" (effects (font (size 1.27 1.27)))) (number "19" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 17.78 270) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "20" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 -17.78 90) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "10" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_oscillator():
    """12MHz CMOS oscillator — 4-pin SMD."""
    return """    (symbol "Oscillator:ASE" (in_bom yes) (on_board yes)
      (property "Reference" "Y" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Value" "12MHz" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "ASE_0_1"
        (rectangle (start -5.08 2.54) (end 5.08 -2.54) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "ASE_1_1"
        (pin power_in line (at -7.62 0 0) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 7.62 -2.54 180) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin output line (at 7.62 0 180) (length 2.54) (name "OUT" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin input line (at -7.62 -2.54 0) (length 2.54) (name "EN" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_psram():
    """LY68L6400 8MB QSPI PSRAM — SOP-8."""
    return """    (symbol "Memory_RAM:LY68L6400" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "LY68L6400" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "LY68L6400_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "LY68L6400_1_1"
        (pin input line (at -10.16 3.81 0) (length 2.54) (name "CE#" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 1.27 0) (length 2.54) (name "SO/SIO1" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 -1.27 0) (length 2.54) (name "SIO2" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 -3.81 0) (length 2.54) (name "VSS" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 3.81 180) (length 2.54) (name "SI/SIO0" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin input line (at 10.16 1.27 180) (length 2.54) (name "SCLK" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 -1.27 180) (length 2.54) (name "SIO3" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 10.16 -3.81 180) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_ice40hx4k():
    """iCE40HX4K-TQ144 FPGA — simplified TQFP-144 with key pins."""
    pins = []
    for i in range(20):
        y = 25.4 - i * 2.54
        pins.append(f'        (pin bidirectional line (at -22.86 {y:.2f} 0) (length 2.54) (name "IOB_{i}" (effects (font (size 1.016 1.016)))) (number "{i+1}" (effects (font (size 1.016 1.016)))))')
    for i in range(20, 40):
        y = 25.4 - (i - 20) * 2.54
        pins.append(f'        (pin bidirectional line (at 22.86 {y:.2f} 180) (length 2.54) (name "IOB_{i}" (effects (font (size 1.016 1.016)))) (number "{i+1}" (effects (font (size 1.016 1.016)))))')
    special = [
        ("VCC", "141", "power_in", -22.86, -27.94),
        ("GND", "142", "power_in", -22.86, -30.48),
        ("CRESET_B", "143", "input", 22.86, -27.94),
        ("CDONE", "144", "output", 22.86, -30.48),
    ]
    for name, num, ptype, x, y in special:
        direction = 0 if x < 0 else 180
        pins.append(f'        (pin {ptype} line (at {x:.2f} {y:.2f} {direction}) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    pins_str = "\n".join(pins)
    return f"""    (symbol "FPGA_Lattice:iCE40HX4K-TQ144" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 30.48 0) (effects (font (size 1.27 1.27))))
      (property "Value" "iCE40HX4K" (at 0 -33.02 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_QFP:TQFP-144_20x20mm_P0.5mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "iCE40HX4K-TQ144_0_1"
        (rectangle (start -20.32 27.94) (end 20.32 -25.4) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "iCE40HX4K-TQ144_1_1"
{pins_str}
      )
    )"""

def lib_symbol_cache_psram():
    """APS6404L 8MB QSPI PSRAM — SOP-8 (FPGA cache)."""
    return """    (symbol "Memory_RAM:APS6404L" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "APS6404L" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "APS6404L_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "APS6404L_1_1"
        (pin input line (at -10.16 3.81 0) (length 2.54) (name "CE#" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 1.27 0) (length 2.54) (name "SO/SIO1" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 -1.27 0) (length 2.54) (name "SIO2" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 -3.81 0) (length 2.54) (name "VSS" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 3.81 180) (length 2.54) (name "SI/SIO0" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin input line (at 10.16 1.27 180) (length 2.54) (name "SCLK" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 -1.27 180) (length 2.54) (name "SIO3" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 10.16 -3.81 180) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_microsd():
    """MicroSD push-push card slot."""
    return """    (symbol "Connector:MicroSD" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "MicroSD" (at 0 -12.7 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_Card:microSD_HC_Molex_104031-0811" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "MicroSD_0_1"
        (rectangle (start -7.62 10.16) (end 7.62 -10.16) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "MicroSD_1_1"
        (pin passive line (at -10.16 7.62 0) (length 2.54) (name "DAT2" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at -10.16 5.08 0) (length 2.54) (name "CD/DAT3/CS" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin input line (at -10.16 2.54 0) (length 2.54) (name "CMD/MOSI" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 0 0) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin input line (at -10.16 -2.54 0) (length 2.54) (name "CLK/SCK" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 -5.08 0) (length 2.54) (name "VSS" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin output line (at -10.16 -7.62 0) (length 2.54) (name "DAT0/MISO" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 10.16 7.62 180) (length 2.54) (name "DAT1" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 10.16 0 180) (length 2.54) (name "SHIELD" (effects (font (size 1.27 1.27)))) (number "SH1" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_usb_c():
    """USB Type-C receptacle — simplified 16-pin."""
    return """    (symbol "Connector:USB_C_Receptacle" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 15.24 0) (effects (font (size 1.27 1.27))))
      (property "Value" "USB_C" (at 0 -15.24 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMount_Horizontal" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "USB_C_Receptacle_0_1"
        (rectangle (start -10.16 12.7) (end 10.16 -12.7) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "USB_C_Receptacle_1_1"
        (pin power_out line (at -12.7 10.16 0) (length 2.54) (name "VBUS" (effects (font (size 1.27 1.27)))) (number "A4" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -12.7 5.08 0) (length 2.54) (name "CC1" (effects (font (size 1.27 1.27)))) (number "A5" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -12.7 2.54 0) (length 2.54) (name "CC2" (effects (font (size 1.27 1.27)))) (number "B5" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -12.7 -2.54 0) (length 2.54) (name "D+" (effects (font (size 1.27 1.27)))) (number "A6" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -12.7 -5.08 0) (length 2.54) (name "D-" (effects (font (size 1.27 1.27)))) (number "A7" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -12.7 -10.16 0) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "A1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 12.7 -10.16 180) (length 2.54) (name "SHIELD" (effects (font (size 1.27 1.27)))) (number "S1" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_grove_i2c():
    """Grove-compatible 4-pin I2C connector (GND, VCC, SDA, SCL) — HY2.0-4P."""
    return """    (symbol "Connector:Grove_I2C" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Grove_I2C" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_JST:JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Grove_I2C_0_1"
        (rectangle (start -5.08 2.54) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Grove_I2C_1_1"
        (pin bidirectional line (at -7.62 0 0) (length 2.54) (name "SDA" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin input line (at -7.62 -2.54 0) (length 2.54) (name "SCL" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 7.62 0 180) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 7.62 -2.54 180) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
      )
    )"""

# ── NEW Library Symbols for v1.0 ───────────────────────────────────

def lib_symbol_w5500():
    """W5500 QFN-48 Ethernet chip."""
    pins = []
    # SPI pins on left side
    spi_pins = [
        ("SCSn", "1", "input", -15.24, 10.16),
        ("SCLK", "2", "input", -15.24, 7.62),
        ("MOSI", "3", "input", -15.24, 5.08),
        ("MISO", "4", "output", -15.24, 2.54),
        ("INTn", "5", "output", -15.24, 0),
        ("RSTn", "33", "input", -15.24, -2.54),
        ("PMODE0", "6", "input", -15.24, -5.08),
        ("PMODE1", "34", "input", -15.24, -7.62),
        ("PMODE2", "35", "input", -15.24, -10.16),
    ]
    for name, num, ptype, x, y in spi_pins:
        pins.append(f'        (pin {ptype} line (at {x:.2f} {y:.2f} 0) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    # Ethernet PHY pins on right side
    phy_pins = [
        ("TXP", "8", "output", 15.24, 10.16),
        ("TXN", "9", "output", 15.24, 7.62),
        ("RXP", "11", "input", 15.24, 5.08),
        ("RXN", "12", "input", 15.24, 2.54),
        ("LINKLED", "28", "output", 15.24, 0),
        ("ACTLED", "29", "output", 15.24, -2.54),
        ("RSVD_10", "10", "passive", 15.24, -5.08),
        ("RSVD_13", "13", "passive", 15.24, -7.62),
        ("RSVD_14", "14", "passive", 15.24, -10.16),
    ]
    for name, num, ptype, x, y in phy_pins:
        direction = 180
        pins.append(f'        (pin {ptype} line (at {x:.2f} {y:.2f} {direction}) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    # Power pins top
    pwr_top = [
        ("VCC", "43", "power_in", -5.08, 15.24),
        ("AVDD_1", "41", "power_in", 0, 15.24),
        ("AVDD_2", "42", "power_in", 5.08, 15.24),
    ]
    for name, num, ptype, x, y in pwr_top:
        pins.append(f'        (pin {ptype} line (at {x:.2f} {y:.2f} 270) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    # GND pins bottom
    gnd_pins = [("GND_7", "7"), ("GND_21", "21"), ("GND_35B", "36"), ("GND_48", "48")]
    for i, (name, num) in enumerate(gnd_pins):
        x = -3.81 + i * 2.54
        pins.append(f'        (pin power_in line (at {x:.2f} -15.24 90) (length 2.54) (name "{name}" (effects (font (size 1.016 1.016)))) (number "{num}" (effects (font (size 1.016 1.016)))))')
    pins_str = "\n".join(pins)
    return f"""    (symbol "Interface_Ethernet:W5500" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 17.78 0) (effects (font (size 1.27 1.27))))
      (property "Value" "W5500" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.15x5.15mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "W5500_0_1"
        (rectangle (start -12.7 12.7) (end 12.7 -12.7) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "W5500_1_1"
{pins_str}
      )
    )"""

def lib_symbol_rj45_poe():
    """RJ45 MagJack with PoE center taps."""
    return """    (symbol "Connector:RJ45_PoE" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "RJ45_PoE" (at 0 -12.7 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_RJ:RJ45_Amphenol_ARJM11D7-805-AB-EW2" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "RJ45_PoE_0_1"
        (rectangle (start -10.16 10.16) (end 10.16 -10.16) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "RJ45_PoE_1_1"
        (pin passive line (at -12.7 7.62 0) (length 2.54) (name "TD+" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at -12.7 5.08 0) (length 2.54) (name "TD-" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin passive line (at -12.7 2.54 0) (length 2.54) (name "RD+" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin passive line (at -12.7 0 0) (length 2.54) (name "RD-" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 12.7 7.62 180) (length 2.54) (name "CT1_45" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 12.7 5.08 180) (length 2.54) (name "CT1_45B" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 12.7 2.54 180) (length 2.54) (name "CT2_78" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 12.7 0 180) (length 2.54) (name "CT2_78B" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 12.7 -5.08 180) (length 2.54) (name "SHIELD" (effects (font (size 1.27 1.27)))) (number "SH" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_poe_pd():
    """Ag9905 PoE PD controller module."""
    return """    (symbol "Power_Management:Ag9905" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Ag9905" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Ag9905_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Ag9905_1_1"
        (pin power_in line (at -10.16 3.81 0) (length 2.54) (name "VIN+" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 1.27 0) (length 2.54) (name "VIN-" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin input line (at -10.16 -1.27 0) (length 2.54) (name "EN" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 10.16 3.81 180) (length 2.54) (name "VOUT+" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_out line (at 10.16 1.27 180) (length 2.54) (name "VOUT-" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin output line (at 10.16 -1.27 180) (length 2.54) (name "PG" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_74hc595():
    """74HC595 shift register SOIC-16."""
    return """    (symbol "74xx:74HC595" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 15.24 0) (effects (font (size 1.27 1.27))))
      (property "Value" "74HC595" (at 0 -15.24 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "74HC595_0_1"
        (rectangle (start -10.16 12.7) (end 10.16 -12.7) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "74HC595_1_1"
        (pin input line (at -12.7 10.16 0) (length 2.54) (name "SER" (effects (font (size 1.27 1.27)))) (number "14" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 7.62 0) (length 2.54) (name "SRCLK" (effects (font (size 1.27 1.27)))) (number "11" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 5.08 0) (length 2.54) (name "RCLK" (effects (font (size 1.27 1.27)))) (number "12" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 2.54 0) (length 2.54) (name "SRCLR" (effects (font (size 1.27 1.27)))) (number "10" (effects (font (size 1.27 1.27)))))
        (pin input line (at -12.7 0 0) (length 2.54) (name "OE" (effects (font (size 1.27 1.27)))) (number "13" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 10.16 180) (length 2.54) (name "QA" (effects (font (size 1.27 1.27)))) (number "15" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 7.62 180) (length 2.54) (name "QB" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 5.08 180) (length 2.54) (name "QC" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 2.54 180) (length 2.54) (name "QD" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 0 180) (length 2.54) (name "QE" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -2.54 180) (length 2.54) (name "QF" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -5.08 180) (length 2.54) (name "QG" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -7.62 180) (length 2.54) (name "QH" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin output line (at 12.7 -10.16 180) (length 2.54) (name "QH_prime" (effects (font (size 1.27 1.27)))) (number "9" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 15.24 270) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "16" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 0 -15.24 90) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_spi_flash():
    """W25Q32 SPI flash SOIC-8."""
    return """    (symbol "Memory_Flash:W25Q32" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "W25Q32" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "W25Q32_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "W25Q32_1_1"
        (pin input line (at -10.16 3.81 0) (length 2.54) (name "CS" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 1.27 0) (length 2.54) (name "DO/IO1" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at -10.16 -1.27 0) (length 2.54) (name "WP/IO2" (effects (font (size 1.27 1.27)))) (number "3" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at -10.16 -3.81 0) (length 2.54) (name "GND" (effects (font (size 1.27 1.27)))) (number "4" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 3.81 180) (length 2.54) (name "DI/IO0" (effects (font (size 1.27 1.27)))) (number "5" (effects (font (size 1.27 1.27)))))
        (pin input line (at 10.16 1.27 180) (length 2.54) (name "CLK" (effects (font (size 1.27 1.27)))) (number "6" (effects (font (size 1.27 1.27)))))
        (pin bidirectional line (at 10.16 -1.27 180) (length 2.54) (name "HOLD/IO3" (effects (font (size 1.27 1.27)))) (number "7" (effects (font (size 1.27 1.27)))))
        (pin power_in line (at 10.16 -3.81 180) (length 2.54) (name "VCC" (effects (font (size 1.27 1.27)))) (number "8" (effects (font (size 1.27 1.27)))))
      )
    )"""

# ── Worker Node Sub-Sheet (RP2354B + PSRAM — SPI Star Slave) ──────

def gen_worker_node():
    s = sch_header("A3", "Worker Node (RP2354B + 8MB PSRAM) — SPI Star Slave")
    s += "  (lib_symbols\n"
    s += lib_symbol_rp2354b() + "\n"
    s += lib_symbol_psram() + "\n"
    s += lib_symbol_cap() + "\n"
    s += lib_symbol_resistor() + "\n"
    s += "  )\n\n"

    ux, uy = 100, 140
    s += place_symbol("MCU_RaspberryPi:RP2354B", "U?", "RP2354B", ux, uy)

    # PSRAM (LY68L6400) connected to QSPI CS1
    psram_x, psram_y = 160, 150
    s += place_symbol("Memory_RAM:LY68L6400", "U?", "LY68L6400_8MB", psram_x, psram_y)
    s += place_symbol("Device:C", "C?", "100nF", psram_x + 15, psram_y)

    # Decoupling caps for RP2354B
    for cx, cy, val in [
        (ux + 35, uy + 25, "100nF"),
        (ux + 35, uy + 20, "1uF"),
    ]:
        s += place_symbol("Device:C", "C?", val, cx, cy)

    # SPI slave labels (from FPGA star)
    s += place_hier_label("SPI_MOSI", 30, 100, 180, "input")
    s += place_hier_label("SPI_MISO", 30, 105, 180, "output")
    s += place_hier_label("SPI_SCK", 30, 110, 180, "input")
    s += place_hier_label("SPI_CS", 30, 115, 180, "input")

    # I2C bootstrap bus
    s += place_hier_label("I2C_SDA", 30, 125, 180, "bidirectional")
    s += place_hier_label("I2C_SCL", 30, 130, 180, "input")

    # Buffered clock input
    s += place_hier_label("CLK_IN", 30, 140, 180, "input")

    # Reset from bootstrap
    s += place_hier_label("RESET", 30, 150, 180, "input")

    # SWD test pads
    s += place_hier_label("SWCLK", 30, 160, 180, "input")
    s += place_hier_label("SWDIO", 30, 165, 180, "bidirectional")

    # Power
    s += place_hier_label("VCC_3V3", 30, 70, 180, "input")
    s += place_hier_label("GND", 30, 75, 180, "input")

    # Wire MCU GPIO to labels
    s += place_wire(ux - 20.32, uy + 58.42 - 0*2.54, 30, uy + 58.42 - 0*2.54)   # GPIO0 -> SPI_MOSI area
    s += place_wire(ux - 20.32, uy + 58.42 - 1*2.54, 30, uy + 58.42 - 1*2.54)   # GPIO1 -> SPI_MISO area
    s += place_wire(ux - 20.32, uy + 58.42 - 2*2.54, 30, uy + 58.42 - 2*2.54)   # GPIO2 -> SPI_SCK area
    s += place_wire(ux - 20.32, uy + 58.42 - 3*2.54, 30, uy + 58.42 - 3*2.54)   # GPIO3 -> SPI_CS area

    # Annotations
    s += place_text("RP2354B Worker Node — SPI Star Slave — 2MB Flash + 8MB PSRAM", 100, 20, 3.0)
    s += place_text("GPIO0: SPI_MOSI | GPIO1: SPI_MISO | GPIO2: SPI_SCK | GPIO3: SPI_CS", 50, 30, 1.27)
    s += place_text("GPIO4: I2C_SDA | GPIO5: I2C_SCL (bootstrap bus)", 50, 35, 1.27)
    s += place_text("GPIO6: CLK_IN (buffered 12MHz clock)", 50, 40, 1.27)
    s += place_text("QSPI CS0: 2MB onboard flash | QSPI CS1: LY68L6400 8MB PSRAM", 50, 45, 1.27)
    s += place_text("RUN pin: RESET from bootstrap expander", 50, 50, 1.27)
    s += place_text("SWD: SWCLK + SWDIO test pads", 50, 55, 1.27)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "worker_node.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── Power Supply Sub-Sheet ──────────────────────────────────────────

def gen_power_supply():
    s = sch_header("A3", "Power Supply — 4-Zone + PoE PD")
    s += "  (lib_symbols\n"
    s += lib_symbol_tps54560() + "\n"
    s += lib_symbol_barrel_jack() + "\n"
    s += lib_symbol_cap() + "\n"
    s += lib_symbol_resistor() + "\n"
    s += lib_symbol_inductor() + "\n"
    s += lib_symbol_poe_pd() + "\n"
    s += "  )\n\n"

    # Screw Terminal for 12V DC
    s += place_symbol("Connector:Screw_Terminal_01x02", "J?", "12V_DC", 40, 40)
    s += place_text("12V DC Screw Terminal (or PoE input)", 40, 32, 1.27)

    # 4x TPS54560 — 4 power zones
    for i in range(4):
        zone_x = 30 + i * 60
        zone_y = 50
        zone_label = f"Z{i+1}"
        s += place_symbol("Regulator_Switching:TPS54560", "U?", f"TPS54560_{zone_label}", zone_x, zone_y)
        s += place_symbol("Device:C", "C?", "10uF", zone_x - 20, zone_y + 15)
        s += place_symbol("Device:C", "C?", "22uF", zone_x + 30, zone_y + 15)
        s += place_symbol("Device:L", "L?", "4.7uH", zone_x + 20, zone_y - 5)
        s += place_symbol("Device:R", "R?", "100K", zone_x - 10, zone_y + 25)
        s += place_symbol("Device:R", "R?", "49.9K", zone_x - 10, zone_y + 35)
        s += place_symbol("Device:C", "C?", "100nF", zone_x + 15, zone_y + 5)
        s += place_text(f"Zone {i+1}: 3.3V / 5A", zone_x, zone_y - 18, 1.27)

    # PoE PD section
    s += place_symbol("Power_Management:Ag9905", "U?", "Ag9905_PoE_PD", 30, 130)
    s += place_symbol("Device:C", "C?", "47uF", 10, 130)
    s += place_symbol("Device:C", "C?", "47uF", 50, 130)
    s += place_text("PoE PD: Ag9905 — 802.3af input -> 12V to Zone 1 VIN", 30, 118, 1.27)

    # Hier labels
    s += place_hier_label("VIN_12V", 260, 40, 0, "input")
    for i in range(4):
        s += place_hier_label(f"VCC_3V3_Z{i+1}", 260, 55 + i * 8, 0, "output")
    s += place_hier_label("GND", 260, 95, 0, "output")
    s += place_hier_label("POE_VIN_P", 30, 155, 180, "input")
    s += place_hier_label("POE_VIN_N", 30, 160, 180, "input")

    s += place_text("Power: 12V DC / PoE -> 4x TPS54560 Buck -> 3.3V / 5A each", 150, 10, 3.0)
    s += place_text("110 nodes @ 120mA = 13.2A + FPGA/SD/support = ~16A total", 150, 20, 2.0)
    s += place_text("4 x 5A = 20A capacity (headroom for spikes)", 150, 28, 2.0)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "power_supply.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── Bootstrap Sub-Sheet (14x PCF8574 CS Mux) ────────────────────────

def gen_bootstrap():
    s = sch_header("A3", "Bootstrap — 14x I2C GPIO Expander (CS Mux)")
    s += "  (lib_symbols\n"
    s += lib_symbol_pcf8574() + "\n"
    s += lib_symbol_cap() + "\n"
    s += lib_symbol_resistor() + "\n"
    s += "  )\n\n"

    # 8x PCF8574 at addresses 0x20-0x27
    # 6x PCF8574A at addresses 0x38-0x3D
    # Place in 2 columns of 7
    col1_x, col2_x = 40, 140
    spacing_y = 30
    chip_idx = 0
    output_idx = 0

    for idx in range(14):
        if idx < 7:
            ux = col1_x
            uy = 60 + idx * spacing_y
        else:
            ux = col2_x
            uy = 60 + (idx - 7) * spacing_y

        if idx < 8:
            chip_type = "PCF8574"
            addr = f"0x{0x20 + idx:02X}"
            a0 = idx & 1
            a1 = (idx >> 1) & 1
            a2 = (idx >> 2) & 1
        else:
            chip_type = "PCF8574A"
            sub = idx - 8
            addr = f"0x{0x38 + sub:02X}"
            a0 = sub & 1
            a1 = (sub >> 1) & 1
            a2 = (sub >> 2) & 1

        s += place_symbol("Interface_I2C:PCF8574", "U?", f"{chip_type}_{addr}", ux, uy)
        s += place_symbol("Device:C", "C?", "100nF", ux + 20, uy - 10)
        s += place_text(f"A0={'V' if a0 else 'G'} A1={'V' if a1 else 'G'} A2={'V' if a2 else 'G'}", ux, uy + 20, 1.0)

        for p in range(8):
            if output_idx < 112:
                s += place_label(f"BST_P{output_idx}", ux + 25, uy + 10.16 - p * 2.54)
                output_idx += 1

    # Shared I2C bus labels
    s += place_hier_label("I2C_SDA", 20, 40, 180, "bidirectional")
    s += place_hier_label("I2C_SCL", 20, 45, 180, "input")

    # Power
    s += place_hier_label("VCC_3V3", 20, 30, 180, "input")
    s += place_hier_label("GND", 20, 35, 180, "input")

    # Output labels for all bootstrap pins
    bst_y = 300
    for n in range(112):
        col = n // 28
        row = n % 28
        s += place_hier_label(f"BST_P{n}", 20 + col * 60, bst_y + row * 5, 180 if col % 2 == 0 else 0, "output")

    s += place_text("Bootstrap Controller — 14x PCF8574/PCF8574A I2C GPIO Expander", 120, 10, 3.0)
    s += place_text("8x PCF8574 (0x20-0x27) + 6x PCF8574A (0x38-0x3D) = 14 chips", 80, 20, 1.5)
    s += place_text("14 x 8 = 112 outputs: 110 for worker RESET + 2 spare", 80, 26, 1.5)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "bootstrap.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── Clock Tree Sub-Sheet ───────────────────────────────────────────

def gen_clock_tree():
    s = sch_header("A3", "Clock Tree — 1x Oscillator + 14x Buffer")
    s += "  (lib_symbols\n"
    s += lib_symbol_oscillator() + "\n"
    s += lib_symbol_74hc244() + "\n"
    s += lib_symbol_cap() + "\n"
    s += "  )\n\n"

    # 12MHz oscillator
    s += place_symbol("Oscillator:ASE", "Y?", "12MHz", 50, 40)
    s += place_symbol("Device:C", "C?", "100nF", 50, 55)
    s += place_text("EN -> VCC (always enabled)", 50, 30, 1.27)

    # 14x 74HC244 buffers in 2 columns of 7
    col1_x, col2_x = 40, 140
    spacing_y = 25

    for idx in range(14):
        if idx < 7:
            ux = col1_x
            uy = 80 + idx * spacing_y
        else:
            ux = col2_x
            uy = 80 + (idx - 7) * spacing_y

        s += place_symbol("74xx:74HC244", "U?", "74HC244", ux, uy)
        s += place_symbol("Device:C", "C?", "100nF", ux + 20, uy - 12)
        s += place_text("1OE=GND, 2OE=GND", ux, uy + 22, 1.0)

        for out in range(8):
            clk_idx = idx * 8 + out
            if clk_idx < NUM_WORKERS:
                s += place_label(f"CLK_OUT_{clk_idx}", ux + 25, uy + 12.7 - out * 2.54)

    # Clock outputs as hier labels
    clk_y = 280
    for n in range(NUM_WORKERS):
        col = n // 28
        row = n % 28
        s += place_hier_label(f"CLK_OUT_{n}", 20 + col * 60, clk_y + row * 5, 180 if col % 2 == 0 else 0, "output")

    # Power
    s += place_hier_label("VCC_3V3", 20, 30, 180, "input")
    s += place_hier_label("GND", 20, 35, 180, "input")

    s += place_text("Clock Fanout Tree — 110 nodes", 120, 10, 3.0)
    s += place_text("1x 12MHz CMOS oscillator -> 14x 74HC244 octal buffers", 80, 20, 1.5)
    s += place_text("14 x 8 = 112 buffered outputs, 110 used for workers", 80, 26, 1.5)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "clock_tree.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── FPGA Subsystem Sub-Sheet ───────────────────────────────────────

def gen_fpga_subsystem():
    s = sch_header("A2", "FPGA Subsystem — iCE40HX4K + W5500 + SD RAID + Cache + CS Mux")
    s += "  (lib_symbols\n"
    s += lib_symbol_ice40hx4k() + "\n"
    s += lib_symbol_w5500() + "\n"
    s += lib_symbol_spi_flash() + "\n"
    s += lib_symbol_microsd() + "\n"
    s += lib_symbol_cache_psram() + "\n"
    s += lib_symbol_74hc595() + "\n"
    s += lib_symbol_cap() + "\n"
    s += lib_symbol_resistor() + "\n"
    s += "  )\n\n"

    # iCE40HX4K FPGA
    fpga_x, fpga_y = 80, 80
    s += place_symbol("FPGA_Lattice:iCE40HX4K-TQ144", "U?", "iCE40HX4K", fpga_x, fpga_y)

    # Decoupling caps for FPGA
    for i in range(4):
        s += place_symbol("Device:C", "C?", "100nF", fpga_x + 30 + i * 8, fpga_y - 20)
    s += place_symbol("Device:C", "C?", "10uF", fpga_x + 30, fpga_y - 28)

    # W5500 QFN-48
    w5500_x, w5500_y = 200, 80
    s += place_symbol("Interface_Ethernet:W5500", "U?", "W5500", w5500_x, w5500_y)
    s += place_symbol("Device:C", "C?", "100nF", w5500_x + 25, w5500_y - 10)
    s += place_symbol("Device:C", "C?", "10uF", w5500_x + 25, w5500_y - 18)

    # W25Q32 SPI flash (FPGA bitstream)
    flash_x, flash_y = 40, 80
    s += place_symbol("Memory_Flash:W25Q32", "U?", "W25Q32", flash_x, flash_y)
    s += place_symbol("Device:C", "C?", "100nF", flash_x + 15, flash_y)

    # 5x MicroSD slots
    for i in range(5):
        sd_x = 80 + i * 30
        sd_y = 170
        s += place_symbol("Connector:MicroSD", "J?", f"MicroSD_{i+1}", sd_x, sd_y)
        s += place_symbol("Device:C", "C?", "100nF", sd_x + 15, sd_y - 8)

    # 2x APS6404L cache PSRAM
    for i in range(2):
        cache_x = 250
        cache_y = 60 + i * 30
        s += place_symbol("Memory_RAM:APS6404L", "U?", f"APS6404L_CACHE{i+1}", cache_x, cache_y)
        s += place_symbol("Device:C", "C?", "100nF", cache_x + 15, cache_y)

    # 14x 74HC595 shift registers in 2 columns of 7
    sr_col1_x, sr_col2_x = 300, 370
    sr_spacing_y = 25
    for idx in range(14):
        if idx < 7:
            sr_x = sr_col1_x
            sr_y = 40 + idx * sr_spacing_y
        else:
            sr_x = sr_col2_x
            sr_y = 40 + (idx - 7) * sr_spacing_y

        s += place_symbol("74xx:74HC595", "U?", f"74HC595_{idx}", sr_x, sr_y)
        s += place_symbol("Device:C", "C?", "100nF", sr_x + 20, sr_y - 10)

        # Label cascade: QH' -> SER of next
        if idx < 13:
            s += place_label(f"SR_CASCADE_{idx}", sr_x + 25, sr_y - 10.16)

        # Output labels: 8 CS lines per shift register
        for bit in range(8):
            cs_idx = idx * 8 + bit
            if cs_idx < NUM_WORKERS:
                s += place_label(f"CS_{cs_idx}", sr_x + 25, sr_y + 10.16 - bit * 2.54)

    # Hier labels — SPI bus to all workers
    s += place_hier_label("SPI_MOSI", 20, 60, 180, "output")
    s += place_hier_label("SPI_MISO", 20, 65, 180, "input")
    s += place_hier_label("SPI_SCK", 20, 70, 180, "output")

    # CS lines — individual chip selects
    cs_y = 230
    for n in range(NUM_WORKERS):
        col = n // 28
        row = n % 28
        s += place_hier_label(f"CS_{n}", 20 + col * 60, cs_y + row * 5, 180 if col % 2 == 0 else 0, "output")

    # Ethernet signals to RJ45
    s += place_hier_label("ETH_TX_P", 420, 60, 0, "output")
    s += place_hier_label("ETH_TX_N", 420, 65, 0, "output")
    s += place_hier_label("ETH_RX_P", 420, 70, 0, "input")
    s += place_hier_label("ETH_RX_N", 420, 75, 0, "input")

    # FPGA I2C management
    s += place_hier_label("I2C_SDA", 20, 80, 180, "bidirectional")
    s += place_hier_label("I2C_SCL", 20, 85, 180, "input")

    # Power
    s += place_hier_label("VCC_3V3", 20, 40, 180, "input")
    s += place_hier_label("GND", 20, 45, 180, "input")

    # Annotations
    s += place_text("FPGA Subsystem — iCE40HX4K + W5500 + SD RAID-0 + Cache + CS Mux", 200, 10, 3.0)
    s += place_text("iCE40HX4K: SPI star master to 110 workers via 74HC595 CS cascade", 100, 20, 1.5)
    s += place_text("W5500: On-board Ethernet connected to FPGA SPI", 100, 26, 1.5)
    s += place_text("W25Q32: FPGA bitstream storage", 100, 32, 1.5)
    s += place_text("5x MicroSD RAID-0 | 2x APS6404L cache | 14x 74HC595 (112 CS lines)", 100, 38, 1.5)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "fpga_subsystem.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── Top-Level Hierarchical Schematic ────────────────────────────────

def gen_top_level():
    s = sch_header("A0", "RP2354 110-Node Network Compute Fabric v1.0")
    s += "  (lib_symbols\n"
    s += lib_symbol_rj45_poe() + "\n"
    s += lib_symbol_usb_c() + "\n"
    s += lib_symbol_grove_i2c() + "\n"
    s += lib_symbol_led() + "\n"
    s += lib_symbol_resistor() + "\n"
    s += lib_symbol_barrel_jack() + "\n"
    s += "  )\n\n"

    s += place_text("RP2354 110-Node Network Compute Fabric v1.0", 400, 20, 5.0)
    s += place_text("110x RP2354B (220 cores) | iCE40HX4K FPGA SPI star | W5500 Ethernet", 400, 35, 2.5)
    s += place_text("4x TPS54560 | 14x PCF8574 | 14x 74HC244 | 14x 74HC595 | PoE 802.3af", 400, 45, 2.0)
    s += place_text("5x MicroSD RAID-0 | 200x200mm 4-layer PCB", 400, 55, 2.0)

    # ── RJ45 MagJack connector ──
    rj45_x, rj45_y = 50, 80
    s += place_symbol("Connector:RJ45_PoE", "J?", "RJ45_PoE", rj45_x, rj45_y)
    s += place_text("RJ45 MagJack with PoE center taps", rj45_x, rj45_y + 15, 1.0)

    # ── USB-C connector ──
    usb_x, usb_y = 50, 130
    s += place_symbol("Connector:USB_C_Receptacle", "J?", "USB_C", usb_x, usb_y)
    s += place_symbol("Device:R", "R?", "5.1k", usb_x + 25, usb_y - 5)
    s += place_symbol("Device:R", "R?", "5.1k", usb_x + 25, usb_y)
    s += place_text("5.1k on CC1/CC2 to GND (UFP sink)", usb_x + 25, usb_y + 8, 1.0)

    # ── Grove I2C connector ──
    s += place_symbol("Connector:Grove_I2C", "J?", "GROVE_I2C", 50, 175)
    s += place_text("I2C bootstrap bus connector", 50, 183, 1.0)

    # ── Screw terminal for 12V ──
    s += place_symbol("Connector:Screw_Terminal_01x02", "J?", "12V_DC", 50, 200)
    s += place_text("12V DC input (or PoE)", 50, 210, 1.0)

    # ── 4x Status LEDs ──
    led_info = [("PWR", "Green"), ("SYS", "Blue"), ("ERR", "Red"), ("NET", "Yellow")]
    for i, (name, color) in enumerate(led_info):
        lx = 600 + i * 30
        ly = 80
        s += place_symbol("Device:LED", "D?", f"{name}_{color}", lx, ly)
        s += place_symbol("Device:R", "R?", "330", lx + 8, ly)
    s += place_text("PWR | SYS | ERR | NET LEDs (330 ohm)", 600, 70, 1.27)

    # ── Power Supply Sheet ──
    pwr_pins = [
        ("VIN_12V", "input", 0, 5, 180),
        ("VCC_3V3_Z1", "output", 80, 5, 0),
        ("VCC_3V3_Z2", "output", 80, 11, 0),
        ("VCC_3V3_Z3", "output", 80, 17, 0),
        ("VCC_3V3_Z4", "output", 80, 23, 0),
        ("GND", "output", 80, 29, 0),
        ("POE_VIN_P", "input", 0, 11, 180),
        ("POE_VIN_N", "input", 0, 17, 180),
    ]
    s += place_hier_sheet("Power_Supply", "power_supply.kicad_sch", 20, 240, 80, 35, pwr_pins)
    s += place_text("4x TPS54560 + PoE PD (Ag9905)", 60, 238, 1.5)

    # ── Bootstrap Sheet ──
    boot_pins = [
        ("I2C_SDA", "bidirectional", 0, 5, 180),
        ("I2C_SCL", "input", 0, 10, 180),
        ("VCC_3V3", "input", 60, 5, 0),
        ("GND", "input", 60, 10, 0),
    ]
    boot_h = 20
    s += place_hier_sheet("Bootstrap", "bootstrap.kicad_sch", 20, 290, 60, boot_h, boot_pins)
    s += place_text("14x PCF8574/PCF8574A (112 bootstrap outputs, 110 used)", 50, 288, 1.5)

    # ── Clock Tree Sheet ──
    clk_pins = [
        ("VCC_3V3", "input", 60, 5, 0),
        ("GND", "input", 60, 10, 0),
    ]
    clk_h = 20
    s += place_hier_sheet("Clock_Tree", "clock_tree.kicad_sch", 20, 325, 60, clk_h, clk_pins)
    s += place_text("1x Oscillator + 14x 74HC244 (112 clocks, 110 used)", 50, 323, 1.5)

    # ── FPGA Subsystem Sheet ──
    fpga_pins = [
        ("SPI_MOSI", "output", 0, 5, 180),
        ("SPI_MISO", "input", 0, 10, 180),
        ("SPI_SCK", "output", 0, 15, 180),
        ("ETH_TX_P", "output", 120, 5, 0),
        ("ETH_TX_N", "output", 120, 10, 0),
        ("ETH_RX_P", "input", 120, 15, 0),
        ("ETH_RX_N", "input", 120, 20, 0),
        ("I2C_SDA", "bidirectional", 0, 20, 180),
        ("I2C_SCL", "input", 0, 25, 180),
        ("VCC_3V3", "input", 120, 25, 0),
        ("GND", "input", 120, 30, 0),
    ]
    # Add CS lines to FPGA sheet
    for n in range(NUM_WORKERS):
        col_offset = 0 if n < 55 else 120
        side = 180 if col_offset == 0 else 0
        row_in_col = n if n < 55 else n - 55
        fpga_pins.append((f"CS_{n}", "output", col_offset, 35 + row_in_col * 3, side))
    fpga_h = 35 + 55 * 3 + 5
    s += place_hier_sheet("FPGA_Subsystem", "fpga_subsystem.kicad_sch",
                          200, 240, 120, fpga_h, fpga_pins)
    s += place_text("iCE40HX4K + W5500 + 5x MicroSD + 2x Cache + 14x 74HC595", 260, 238, 1.5)

    # ── 110x Worker Node Sheets in 10 rows x 11 cols ──
    for worker_idx in range(NUM_WORKERS):
        row = worker_idx // 11
        col = worker_idx % 11
        sheet_x = 60 + col * 20
        sheet_y = 40 + row * 25

        node_pins = [
            ("SPI_MOSI", "input", 0, 3, 180),
            ("SPI_MISO", "output", 0, 6, 180),
            ("SPI_SCK", "input", 0, 9, 180),
            ("SPI_CS", "input", 0, 12, 180),
            ("I2C_SDA", "bidirectional", 15, 3, 0),
            ("I2C_SCL", "input", 15, 6, 0),
            ("CLK_IN", "input", 15, 9, 0),
            ("VCC_3V3", "input", 15, 12, 0),
            ("GND", "input", 15, 15, 0),
            ("RESET", "input", 15, 18, 0),
        ]
        s += place_hier_sheet(f"Worker_{worker_idx}", "worker_node.kicad_sch",
                              sheet_x, sheet_y, 15, 22, node_pins)
        s += place_text(f"W{worker_idx}", sheet_x + 7, sheet_y + 23, 0.8)

    # ── Global labels connecting subsystems ──
    s += place_global_label("VCC_3V3", 400, 160, 0, "input")
    s += place_global_label("I2C_SDA", 400, 170, 0, "bidirectional")
    s += place_global_label("I2C_SCL", 400, 175, 0, "input")
    s += place_global_label("SPI_MOSI", 400, 185, 0, "bidirectional")
    s += place_global_label("SPI_MISO", 400, 190, 0, "bidirectional")
    s += place_global_label("SPI_SCK", 400, 195, 0, "input")

    s += place_text("Global buses: VCC_3V3, I2C (SDA/SCL), SPI (MOSI/MISO/SCK)", 400, 200, 1.5)
    s += place_text("CS_0..CS_109: individual chip selects from FPGA 74HC595 cascade", 400, 206, 1.5)
    s += place_text("ETH signals: FPGA subsystem <-> RJ45 MagJack", 400, 212, 1.5)
    s += place_text("PoE center taps: RJ45 CT1/CT2 -> Power Supply Ag9905", 400, 218, 1.5)

    s += "\n  (sheet_instances\n    (path \"/\" (page \"1\"))\n  )\n"
    s += sch_footer()
    path = os.path.join(OUTPUT_DIR, "cluster_board.kicad_sch")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── PCB Layout ──────────────────────────────────────────────────────

def gen_pcb():
    board_w = BOARD_W
    board_h = BOARD_H

    s = f"""(kicad_pcb
  (version 20240108)
  (generator "copilot_cluster_gen")
  (generator_version "2.0")
  (general
    (thickness 1.6)
    (legacy_teardrops no)
  )
  (paper "A3")
  (title_block
    (title "RP2354 110-Node Network Compute Fabric v1.0")
    (rev "1.0")
    (company "Cluster Tile Project")
  )
  (layers
    (0 "F.Cu" signal)
    (1 "In1.Cu" signal)
    (2 "In2.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user "B.Mask")
    (39 "F.Mask" user "F.Mask")
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user "B.Fabrication")
    (49 "F.Fab" user "F.Fabrication")
  )
  (setup
    (pad_to_mask_clearance 0)
    (allow_soldermask_bridges_in_footprints no)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
    )
  )
  (net 0 "")
  (net 1 "VCC_3V3")
  (net 2 "GND")
  (net 3 "VIN_12V")
  (net 4 "SPI_MOSI")
  (net 5 "SPI_MISO")
  (net 6 "SPI_SCK")
  (net 7 "I2C_SDA")
  (net 8 "I2C_SCL")
"""

    # CS nets
    net_num = 9
    for n in range(NUM_WORKERS):
        s += f'  (net {net_num} "CS_{n}")\n'
        net_num += 1

    # Ethernet nets
    for sig in ["ETH_TX_P", "ETH_TX_N", "ETH_RX_P", "ETH_RX_N"]:
        s += f'  (net {net_num} "{sig}")\n'
        net_num += 1

    # Power zone nets
    for z in range(1, 5):
        s += f'  (net {net_num} "VCC_3V3_Z{z}")\n'
        net_num += 1

    # Board outline with rounded corners
    s += f"""
  (gr_rect (start 0 0) (end {board_w} {board_h})
    (stroke (width 0.15) (type default))
    (fill none)
    (layer "Edge.Cuts")
    (uuid "{nuuid()}")
  )
"""

    # Mounting holes (4x M3, 5mm inset)
    for mx, my in [(5, 5), (195, 5), (5, 195), (195, 195)]:
        s += f"""  (footprint "MountingHole:MountingHole_3.2mm_M3"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {mx} {my})
    (property "Reference" "H{mx}{my}" (at 0 -3 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
    (property "Value" "MountingHole" (at 0 3 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
  )
"""

    # ── 110x RP2354B QFN-80 in 10 rows x 11 cols on F.Cu ──
    col_spacing = 15
    row_spacing = 15
    start_x = 18
    start_y = 35
    fp_qfn = "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP5.45x5.45mm"
    fp_psram = "Package_SO:SOP-8_3.9x4.9mm_P1.27mm"

    for idx in range(NUM_WORKERS):
        col = idx % 11
        row = idx // 11
        x = start_x + col * col_spacing
        y = start_y + row * row_spacing

        # QFN-80 on front
        s += f"""  (footprint "{fp_qfn}"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {x:.1f} {y:.1f})
    (property "Reference" "U_W{idx}" (at 0 -5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.5 0.5) (thickness 0.08))))
    (property "Value" "RP2354B" (at 0 5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.5 0.5) (thickness 0.08))))
  )
"""
        # PSRAM SOP-8 on back, directly under QFN-80
        s += f"""  (footprint "{fp_psram}"
    (layer "B.Cu")
    (uuid "{nuuid()}")
    (at {x:.1f} {y:.1f})
    (property "Reference" "U_PSRAM{idx}" (at 0 -4 0) (layer "B.SilkS") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
    (property "Value" "LY68L6400" (at 0 4 0) (layer "B.Fab") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
  )
"""

    # ── iCE40HX4K TQFP-144 ──
    fpga_x = 30
    fpga_y = board_h - 35
    s += f"""  (footprint "Package_QFP:TQFP-144_20x20mm_P0.5mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {fpga_x} {fpga_y})
    (property "Reference" "U_FPGA" (at 0 -12 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
    (property "Value" "iCE40HX4K" (at 0 12 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
  )
"""

    # ── W5500 QFN-48 ──
    w5500_x = 50
    w5500_y = board_h - 15
    s += f"""  (footprint "Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.15x5.15mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {w5500_x} {w5500_y})
    (property "Reference" "U_W5500" (at 0 -5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "W5500" (at 0 5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── RJ45 at edge ──
    rj45_x = 10
    rj45_y = board_h - 15
    s += f"""  (footprint "Connector_RJ:RJ45_Amphenol_ARJM11D7-805-AB-EW2"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {rj45_x} {rj45_y})
    (property "Reference" "J_RJ45" (at 0 -8 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "RJ45_PoE" (at 0 8 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── PoE PD (Ag9905) ──
    poe_x = 30
    poe_y = board_h - 15
    s += f"""  (footprint "Package_SO:SOP-8_3.9x4.9mm_P1.27mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {poe_x} {poe_y})
    (property "Reference" "U_POE" (at 0 -4 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
    (property "Value" "Ag9905" (at 0 4 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
  )
"""

    # ── SPI Flash (W25Q32) ──
    spiflash_x = 60
    spiflash_y = board_h - 35
    s += f"""  (footprint "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {spiflash_x} {spiflash_y})
    (property "Reference" "U_FLASH" (at 0 -4 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
    (property "Value" "W25Q32" (at 0 4 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
  )
"""

    # ── 5x MicroSD ──
    for i in range(5):
        sd_x = 80 + i * 22
        sd_y = board_h - 15
        s += f"""  (footprint "Connector_Card:microSD_HC_Molex_104031-0811"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {sd_x} {sd_y})
    (property "Reference" "J_SD{i+1}" (at 0 -8 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "MicroSD_{i+1}" (at 0 8 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── 2x APS6404L cache PSRAM ──
    for i in range(2):
        cx = 70 + i * 15
        cy = board_h - 35
        s += f"""  (footprint "{fp_psram}"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {cx} {cy})
    (property "Reference" "U_CACHE{i+1}" (at 0 -4 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
    (property "Value" "APS6404L" (at 0 4 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
  )
"""

    # ── 14x 74HC595 near FPGA ──
    for i in range(14):
        sr_x = 5 + i * 12
        sr_y = board_h - 25
        s += f"""  (footprint "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {sr_x:.1f} {sr_y:.1f})
    (property "Reference" "U_SR{i}" (at 0 -6 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
    (property "Value" "74HC595" (at 0 6 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
  )
"""

    # ── 14x PCF8574 left side column ──
    for idx in range(14):
        px = 5
        py = 35 + idx * 10
        s += f"""  (footprint "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {px:.1f} {py:.1f})
    (property "Reference" "U_BST{idx}" (at 0 -6 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
    (property "Value" "PCF8574" (at 0 6 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
  )
"""

    # ── 14x 74HC244 right side column ──
    for idx in range(14):
        bx = 195
        by = 35 + idx * 10
        s += f"""  (footprint "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {bx:.1f} {by:.1f})
    (property "Reference" "U_CLK{idx}" (at 0 -4 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
    (property "Value" "74HC244" (at 0 4 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.4 0.4) (thickness 0.06))))
  )
"""

    # ── 4x TPS54560 along top edge ──
    for reg_idx in range(4):
        rx = 30 + reg_idx * 40
        ry = 5
        s += f"""  (footprint "Package_SO:HSOP-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x3.1mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {rx} {ry})
    (property "Reference" "U_PWR{reg_idx+1}" (at 0 -5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "TPS54560_Z{reg_idx+1}" (at 0 5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── Screw terminal top-left ──
    s += f"""  (footprint "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-2-5.0-H_1x02_P5.00mm_Horizontal"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at 10 10)
    (property "Reference" "J_DC" (at 0 -6 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
    (property "Value" "12V_DC" (at 0 6 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 1 1) (thickness 0.15))))
  )
"""

    # ── USB-C bottom edge center ──
    s += f"""  (footprint "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMount_Horizontal"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at 100 198)
    (property "Reference" "J_USB" (at 0 -5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "USB_C" (at 0 5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── 4x LEDs top edge ──
    for i, name in enumerate(["PWR", "SYS", "ERR", "NET"]):
        lx = 100 + i * 10
        ly = 5
        s += f"""  (footprint "LED_SMD:LED_0603_1608Metric"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {lx} {ly})
    (property "Reference" "D_{name}" (at 0 -2.5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
    (property "Value" "{name}" (at 0 2.5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
  )
"""

    # ── Grove I2C connector near edge ──
    s += f"""  (footprint "Connector_JST:JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at 190 198)
    (property "Reference" "J_GROVE" (at 0 -5 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
    (property "Value" "Grove_I2C" (at 0 5 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.8 0.8) (thickness 0.12))))
  )
"""

    # ── 12MHz Oscillator near center ──
    osc_x = board_w / 2
    osc_y = board_h / 2
    s += f"""  (footprint "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm"
    (layer "F.Cu")
    (uuid "{nuuid()}")
    (at {osc_x:.1f} {osc_y:.1f})
    (property "Reference" "Y1" (at 0 -3 0) (layer "F.SilkS") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
    (property "Value" "12MHz" (at 0 3 0) (layer "F.Fab") (uuid "{nuuid()}") (effects (font (size 0.6 0.6) (thickness 0.1))))
  )
"""

    # ── Board silkscreen labels ──
    s += f"""  (gr_text "RP2354 110-Node Network Compute Fabric v1.0" (at {board_w/2} {board_h - 3})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 2 2) (thickness 0.25)))
  )
  (gr_text "WORKERS (10x11 = 110)" (at {start_x + 5*col_spacing} {start_y - 5})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 1.2 1.2) (thickness 0.15)))
  )
  (gr_text "PSRAM (110x SOP-8 on B.Cu)" (at {start_x + 5*col_spacing} {start_y - 2})
    (layer "B.SilkS") (uuid "{nuuid()}")
    (effects (font (size 1 1) (thickness 0.15)))
  )
  (gr_text "POWER (12V/PoE) 4x TPS54560" (at 80 3)
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 1 1) (thickness 0.15)))
  )
  (gr_text "BOOTSTRAP (14x PCF8574)" (at 5 30)
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.6 0.6) (thickness 0.1)))
  )
  (gr_text "CLK TREE (14x 74HC244)" (at 195 30)
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.6 0.6) (thickness 0.1)))
  )
  (gr_text "FPGA iCE40HX4K" (at {fpga_x} {fpga_y - 15})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 1 1) (thickness 0.15)))
  )
  (gr_text "W5500 Ethernet" (at {w5500_x} {w5500_y - 8})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.8 0.8) (thickness 0.12)))
  )
  (gr_text "RJ45 PoE" (at {rj45_x} {rj45_y - 8})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.8 0.8) (thickness 0.12)))
  )
  (gr_text "MicroSD x5 RAID-0" (at 120 {board_h - 8})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.8 0.8) (thickness 0.12)))
  )
  (gr_text "USB-C" (at 100 {board_h - 6})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.8 0.8) (thickness 0.12)))
  )
  (gr_text "74HC595 x14 CS Cascade" (at 90 {board_h - 22})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.6 0.6) (thickness 0.1)))
  )
  (gr_text "4-LAYER PCB: F.Cu / In1.Cu / In2.Cu / B.Cu" (at {board_w/2} {board_h - 27})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 1 1) (thickness 0.15)))
  )
"""

    # Node number labels on silkscreen
    for idx in range(NUM_WORKERS):
        col = idx % 11
        row = idx // 11
        x = start_x + col * col_spacing
        y = start_y + row * row_spacing + 6
        s += f"""  (gr_text "W{idx}" (at {x:.1f} {y:.1f})
    (layer "F.SilkS") (uuid "{nuuid()}")
    (effects (font (size 0.35 0.35) (thickness 0.06)))
  )
"""

    s += ")\n"
    path = os.path.join(OUTPUT_DIR, "cluster_board.kicad_pcb")
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ── Main ────────────────────────────────────────────────────────────

def main():
    print("=" * 70)
    print("RP2354 110-Node Network Compute Fabric v1.0")
    print("  110x RP2354B + iCE40HX4K FPGA SPI star + on-board W5500 Ethernet")
    print("  4x TPS54560 | 14x PCF8574 | 14x 74HC244 | 14x 74HC595")
    print("  PoE 802.3af via Ag9905 | 5x MicroSD RAID-0 | 200x200mm")
    print("=" * 70)
    gen_project()
    gen_worker_node()
    gen_power_supply()
    gen_bootstrap()
    gen_clock_tree()
    gen_fpga_subsystem()
    gen_top_level()
    gen_pcb()
    print("Done — all files written to", OUTPUT_DIR)

if __name__ == "__main__":
    main()
