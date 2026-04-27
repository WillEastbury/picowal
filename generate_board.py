#!/usr/bin/env python3
"""Hydra Mesh v6.0 -- Ti180 FPGA GPU Compute Node + Rack Backplane
Generates KiCad 8 files for compute node + backplane."""

import json, os

OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

_uuid_counter = 0

def nuuid():
    global _uuid_counter
    _uuid_counter += 1
    return f"{_uuid_counter:08x}-0000-4000-8000-{_uuid_counter:012x}"

def reset_uuids():
    global _uuid_counter
    _uuid_counter = 0

# == KiCad project file ==

def gen_project(name, out_dir):
    proj = {
        "meta": {"filename": f"{name}.kicad_pro", "version": 1},
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
    path = os.path.join(out_dir, f"{name}.kicad_pro")
    with open(path, "w") as f:
        json.dump(proj, f, indent=2)
    print(f"  wrote {path}")

# == Schematic helpers ==

def sch_header(paper="A1", title="", rev="6.0"):
    u = nuuid()
    lines = [
        "(kicad_sch",
        "  (version 20231120)",
        '  (generator "hydra_v6_gen")',
        '  (generator_version "6.0")',
        f'  (uuid "{u}")',
        f'  (paper "{paper}")',
        "  (title_block",
        f'    (title "{title}")',
        f'    (rev "{rev}")',
        '    (company "Hydra Mesh v6.0")',
        "  )",
    ]
    return "\n".join(lines) + "\n"

def sch_footer():
    return ")\n"

def place_symbol(lib_id, ref, value, x, y, angle=0, unit=1, extra_props=None):
    u = nuuid()
    props = ""
    if extra_props:
        for k, v in extra_props.items():
            props += f'\n    (property "{k}" "{v}" (at {x} {y-5.08} 0) (effects (font (size 1.27 1.27)) hide))'
    lines = [
        f'  (symbol (lib_id "{lib_id}") (at {x:.2f} {y:.2f} {angle}) (unit {unit})',
        "    (in_bom yes) (on_board yes) (dnp no)",
        f'    (uuid "{u}")',
        f'    (property "Reference" "{ref}" (at {x:.2f} {y+2.54:.2f} 0) (effects (font (size 1.27 1.27))))',
        f'    (property "Value" "{value}" (at {x:.2f} {y-2.54:.2f} 0) (effects (font (size 1.27 1.27)))){props}',
        "  )",
    ]
    return "\n".join(lines) + "\n"

def place_wire(x1, y1, x2, y2):
    return f'  (wire (pts (xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f})) (uuid "{nuuid()}"))\n'

def place_label(name, x, y, angle=0):
    return f'  (label "{name}" (at {x:.2f} {y:.2f} {angle}) (uuid "{nuuid()}") (effects (font (size 1.27 1.27))))\n'

def place_global_label(name, x, y, angle=0, shape="bidirectional"):
    return f'  (global_label "{name}" (shape {shape}) (at {x:.2f} {y:.2f} {angle}) (uuid "{nuuid()}") (effects (font (size 1.27 1.27))))\n'

def place_text(text, x, y, size=2.54):
    return f'  (text "{text}" (at {x:.2f} {y:.2f} 0) (uuid "{nuuid()}") (effects (font (size {size} {size}))))\n'

# == Pin helper ==

def _pin(ptype, direction, x, y, name, number, font_size=1.27):
    return (
        f'        (pin {ptype} line (at {x:.2f} {y:.2f} {direction}) (length 2.54) '
        f'(name "{name}" (effects (font (size {font_size} {font_size})))) '
        f'(number "{number}" (effects (font (size {font_size} {font_size})))))'
    )

# == Library Symbols ==

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

def lib_symbol_cap_0805():
    return """    (symbol "Device:C_0805" (pin_numbers hide) (pin_names (offset 0.254)) (in_bom yes) (on_board yes)
      (property "Reference" "C" (at 1.524 0 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Value" "C" (at 1.524 -2.54 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Footprint" "Capacitor_SMD:C_0805_2012Metric" (at 0.9652 -3.81 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "C_0805_0_1"
        (polyline (pts (xy -1.524 -0.508) (xy 1.524 -0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
        (polyline (pts (xy -1.524 0.508) (xy 1.524 0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
      )
      (symbol "C_0805_1_1"
        (pin passive line (at 0 2.54 270) (length 2.032) (name "~" (effects (font (size 1.27 1.27)))) (number "1" (effects (font (size 1.27 1.27)))))
        (pin passive line (at 0 -2.54 90) (length 2.032) (name "~" (effects (font (size 1.27 1.27)))) (number "2" (effects (font (size 1.27 1.27)))))
      )
    )"""

def lib_symbol_cap_1206():
    return """    (symbol "Device:C_1206" (pin_numbers hide) (pin_names (offset 0.254)) (in_bom yes) (on_board yes)
      (property "Reference" "C" (at 1.524 0 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Value" "C" (at 1.524 -2.54 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Footprint" "Capacitor_SMD:C_1206_3216Metric" (at 0.9652 -3.81 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "C_1206_0_1"
        (polyline (pts (xy -1.524 -0.508) (xy 1.524 -0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
        (polyline (pts (xy -1.524 0.508) (xy 1.524 0.508)) (stroke (width 0.3048) (type default)) (fill (type none)))
      )
      (symbol "C_1206_1_1"
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

def lib_symbol_ti180():
    pins = []
    lpddr = ["DQ0","DQ1","DQ2","DQ3","DQ4","DQ5","DQ6","DQ7",
             "DQ8","DQ9","DQ10","DQ11","DQ12","DQ13","DQ14","DQ15",
             "CA0","CA1","CA2","CA3","CA4","CA5",
             "DDR_CK","DDR_CKE","DDR_CS","DDR_ODT","DDR_RESET"]
    for i, name in enumerate(lpddr):
        pins.append(_pin("bidirectional", 0, -27.94, 55.88 - i * 2.54, name, f"A{i+1}", 1.016))
    spi = [("FLASH_SCK","C1"),("FLASH_MOSI","C2"),("FLASH_MISO","C3"),("FLASH_CS","C4")]
    for i, (name, num) in enumerate(spi):
        pins.append(_pin("bidirectional", 0, -27.94, -15.24 - i * 2.54, name, num, 1.016))
    serdes = [("TX0P","B1"),("TX0N","B2"),("RX0P","B3"),("RX0N","B4"),
              ("TX1P","B5"),("TX1N","B6"),("RX1P","B7"),("RX1N","B8")]
    for i, (name, num) in enumerate(serdes):
        pins.append(_pin("bidirectional", 180, 27.94, 55.88 - i * 2.54, name, num, 1.016))
    pcie = [("PCIE_TXP0","D1"),("PCIE_TXN0","D2"),("PCIE_RXP0","D3"),("PCIE_RXN0","D4"),
            ("PCIE_TXP1","D5"),("PCIE_TXN1","D6"),("PCIE_RXP1","D7"),("PCIE_RXN1","D8"),
            ("PCIE_TXP2","D9"),("PCIE_TXN2","D10"),("PCIE_RXP2","D11"),("PCIE_RXN2","D12"),
            ("PCIE_TXP3","D13"),("PCIE_TXN3","D14"),("PCIE_RXP3","D15"),("PCIE_RXN3","D16"),
            ("PCIE_REFCLK_P","D17"),("PCIE_REFCLK_N","D18")]
    for i, (name, num) in enumerate(pcie):
        pins.append(_pin("bidirectional", 180, 27.94, 33.02 - i * 2.54, name, num, 1.016))
    for i in range(8):
        pins.append(_pin("bidirectional", 180, 27.94, -15.24 - i * 2.54, f"GPIO{i}", f"G{i+1}", 1.016))
    jtag_cfg = [("TCK","E1","input"),("TMS","E2","input"),("TDI","E3","input"),("TDO","E4","output"),
                ("PROGRAMN","E5","input"),("DONE","E6","output")]
    for i, (name, num, pt) in enumerate(jtag_cfg):
        pins.append(_pin(pt, 180, 27.94, -36.58 - i * 2.54, name, num, 1.016))
    pwr = [("VCC_CORE","P1","power_in"),("VCC_IO","P2","power_in"),
           ("VCC_AUX","P3","power_in"),("GND","P4","power_in"),
           ("GND","P5","power_in"),("GND","P6","power_in"),("GND","P7","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 27.94, -50.8 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "FPGA_Efinix:Ti180" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 60.96 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Ti180" (at 0 -68.58 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-484_1.0mm_22x22_23.0x23.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Ti180_0_1"
        (rectangle (start -25.4 58.42) (end 25.4 -66.04) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Ti180_1_1"
{pins_str}
      )
    )"""

def lib_symbol_lpddr4():
    pins = []
    dq = [f"DQ{i}" for i in range(16)]
    ca = [f"CA{i}" for i in range(6)]
    ctrl = ["CK_t","CK_c","CS","CKE","ODT","RESET_n"]
    left_pins = dq + ca + ctrl
    for i, name in enumerate(left_pins):
        pins.append(_pin("bidirectional", 0, -15.24, 33.02 - i * 2.54, name, f"M{i+1}", 1.016))
    pwr = [("VDD1","V1","power_in"),("VDD2","V2","power_in"),
           ("VDDQ","V3","power_in"),("VSS","V4","power_in"),
           ("VSS","V5","power_in"),("VSS","V6","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 15.24, 5.08 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Memory_DDR:LPDDR4_1GB" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 38.1 0) (effects (font (size 1.27 1.27))))
      (property "Value" "LPDDR4_1GB" (at 0 -40.64 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-200_0.8mm_14x14_12.0x10.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "LPDDR4_1GB_0_1"
        (rectangle (start -12.7 35.56) (end 12.7 -38.1) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "LPDDR4_1GB_1_1"
{pins_str}
      )
    )"""

def lib_symbol_w25q128():
    pins = "\n".join([
        _pin("input", 0, -10.16, 3.81, "CS", "1"),
        _pin("bidirectional", 0, -10.16, 1.27, "DO/IO1", "2"),
        _pin("bidirectional", 0, -10.16, -1.27, "WP/IO2", "3"),
        _pin("power_in", 0, -10.16, -3.81, "GND", "4"),
        _pin("bidirectional", 180, 10.16, 3.81, "DI/IO0", "5"),
        _pin("input", 180, 10.16, 1.27, "CLK", "6"),
        _pin("bidirectional", 180, 10.16, -1.27, "HOLD/IO3", "7"),
        _pin("power_in", 180, 10.16, -3.81, "VCC", "8"),
    ])
    return f"""    (symbol "Memory_Flash:W25Q128JV" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "W25Q128JV" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "W25Q128JV_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "W25Q128JV_1_1"
{pins}
      )
    )"""

def lib_symbol_arc_a380():
    pins = []
    for lane in range(4):
        y = 22.86 - lane * 5.08
        pins.append(_pin("bidirectional", 0, -17.78, y, f"PCIE_TXP{lane}", f"T{lane*2+1}", 1.016))
        pins.append(_pin("bidirectional", 0, -17.78, y - 2.54, f"PCIE_TXN{lane}", f"T{lane*2+2}", 1.016))
    for lane in range(4):
        y = 0 - lane * 5.08
        pins.append(_pin("bidirectional", 0, -17.78, y, f"PCIE_RXP{lane}", f"R{lane*2+1}", 1.016))
        pins.append(_pin("bidirectional", 0, -17.78, y - 2.54, f"PCIE_RXN{lane}", f"R{lane*2+2}", 1.016))
    pins.append(_pin("input", 0, -17.78, -22.86, "REFCLK_P", "CK1", 1.016))
    pins.append(_pin("input", 0, -17.78, -25.4, "REFCLK_N", "CK2", 1.016))
    pins.append(_pin("input", 0, -17.78, -27.94, "PERST_n", "RST", 1.016))
    pins.append(_pin("power_in", 180, 17.78, 22.86, "VCC_GPU", "PG1", 1.016))
    pins.append(_pin("power_in", 180, 17.78, 20.32, "VCC_GDDR", "PG2", 1.016))
    pins.append(_pin("power_in", 180, 17.78, -22.86, "GND", "GN1", 1.016))
    pins.append(_pin("power_in", 180, 17.78, -25.4, "GND", "GN2", 1.016))
    pins.append(_pin("power_in", 180, 17.78, -27.94, "GND", "GN3", 1.016))
    pins.append(_pin("power_in", 180, 17.78, -30.48, "GND", "GN4", 1.016))
    pins.append(_pin("passive", 180, 17.78, -33.02, "THERMAL_PAD", "TP1", 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "GPU:Arc_A380" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 27.94 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Arc_A380" (at 0 -38.1 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-2660_45x37.5mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Arc_A380_0_1"
        (rectangle (start -15.24 25.4) (end 15.24 -35.56) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Arc_A380_1_1"
{pins_str}
      )
    )"""

def lib_symbol_pcie_switch_4port():
    pins = []
    up = [("US_TXP0","U1"),("US_TXN0","U2"),("US_RXP0","U3"),("US_RXN0","U4"),
          ("US_TXP1","U5"),("US_TXN1","U6"),("US_RXP1","U7"),("US_RXN1","U8"),
          ("US_TXP2","U9"),("US_TXN2","U10"),("US_RXP2","U11"),("US_RXN2","U12"),
          ("US_TXP3","U13"),("US_TXN3","U14"),("US_RXP3","U15"),("US_RXN3","U16"),
          ("US_REFCLK_P","U17"),("US_REFCLK_N","U18")]
    for i, (name, num) in enumerate(up):
        pins.append(_pin("bidirectional", 0, -22.86, 43.18 - i * 2.54, name, num, 1.016))
    for port in range(4):
        for j, suffix in enumerate(["TXP","TXN","RXP","RXN"]):
            name = f"DS{port}_{suffix}"
            num = f"D{port*4+j+1}"
            y = -5.08 - port * 12.7 - j * 2.54
            pins.append(_pin("bidirectional", 180, 22.86, y, name, num, 1.016))
    pwr = [("VCC_CORE","P1","power_in"),("VCC_IO","P2","power_in"),
           ("GND","P3","power_in"),("GND","P4","power_in"),
           ("GND","P5","power_in"),("GND","P6","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 0, -22.86, -53.34 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Interface_PCIe:PI7C9X2G404SL" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 48.26 0) (effects (font (size 1.27 1.27))))
      (property "Value" "PI7C9X2G404SL" (at 0 -68.58 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-176_15x15mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "PI7C9X2G404SL_0_1"
        (rectangle (start -20.32 45.72) (end 20.32 -66.04) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "PI7C9X2G404SL_1_1"
{pins_str}
      )
    )"""

def lib_symbol_ir3842():
    pins = "\n".join([
        _pin("power_in", 0, -12.7, 7.62, "VIN", "1"),
        _pin("input", 0, -12.7, 5.08, "EN", "2"),
        _pin("output", 0, -12.7, 2.54, "BOOT", "3"),
        _pin("output", 0, -12.7, 0, "PH", "4"),
        _pin("input", 180, 12.7, 7.62, "FB", "5"),
        _pin("passive", 180, 12.7, 5.08, "COMP", "6"),
        _pin("output", 180, 12.7, 2.54, "PGOOD", "7"),
        _pin("power_in", 180, 12.7, 0, "GND", "8"),
        _pin("power_in", 180, 12.7, -2.54, "EPAD", "9"),
    ])
    return f"""    (symbol "Regulator_Switching:IR3842" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "IR3842" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-16-1EP_5x5mm_P0.65mm_EP3.35x3.35mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "IR3842_0_1"
        (rectangle (start -10.16 10.16) (end 10.16 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "IR3842_1_1"
{pins}
      )
    )"""

def lib_symbol_tlv62130():
    pins = "\n".join([
        _pin("power_in", 0, -10.16, 5.08, "VIN", "1"),
        _pin("input", 0, -10.16, 2.54, "EN", "2"),
        _pin("output", 0, -10.16, 0, "SW", "3"),
        _pin("input", 0, -10.16, -2.54, "FB", "4"),
        _pin("output", 180, 10.16, 5.08, "PG", "5"),
        _pin("power_in", 180, 10.16, 2.54, "AGND", "6"),
        _pin("power_in", 180, 10.16, 0, "PGND", "7"),
        _pin("output", 180, 10.16, -2.54, "VOS", "8"),
    ])
    return f"""    (symbol "Regulator_Switching:TLV62130" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 10.16 0) (effects (font (size 1.27 1.27))))
      (property "Value" "TLV62130" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-16-1EP_3x3mm_P0.5mm_EP1.68x1.68mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "TLV62130_0_1"
        (rectangle (start -7.62 7.62) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "TLV62130_1_1"
{pins}
      )
    )"""

def lib_symbol_ap2112k():
    pins = "\n".join([
        _pin("power_in", 0, -10.16, 2.54, "VIN", "1"),
        _pin("power_in", 0, -10.16, -2.54, "GND", "2"),
        _pin("input", 0, -10.16, 0, "EN", "3"),
        _pin("power_out", 180, 10.16, 2.54, "VOUT", "5"),
        _pin("passive", 180, 10.16, -2.54, "NC", "4"),
    ])
    return f"""    (symbol "Regulator_Linear:AP2112K" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "AP2112K" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_TO_SOT_SMD:SOT-23-5" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "AP2112K_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "AP2112K_1_1"
{pins}
      )
    )"""

def lib_symbol_osc_diff():
    pins = "\n".join([
        _pin("power_in", 0, -7.62, 2.54, "VCC", "4"),
        _pin("power_in", 0, -7.62, -2.54, "GND", "2"),
        _pin("output", 180, 7.62, 2.54, "OUT_P", "3"),
        _pin("output", 180, 7.62, -2.54, "OUT_N", "1"),
    ])
    return f"""    (symbol "Oscillator:ASFL1_100MHz" (in_bom yes) (on_board yes)
      (property "Reference" "Y" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "100MHz" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "ASFL1_100MHz_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "ASFL1_100MHz_1_1"
{pins}
      )
    )"""

def lib_symbol_serdes_conn():
    pin_list = []
    pn = 1
    for pair in range(2):
        pin_list.append(_pin("bidirectional", 180, 12.7, 48.26 - pair * 12.7, f"SERDES_TXP{pair}", str(pn))); pn += 1
        pin_list.append(_pin("bidirectional", 180, 12.7, 48.26 - pair * 12.7 - 2.54, f"SERDES_TXN{pair}", str(pn))); pn += 1
        pin_list.append(_pin("bidirectional", 180, 12.7, 48.26 - pair * 12.7 - 5.08, f"SERDES_RXP{pair}", str(pn))); pn += 1
        pin_list.append(_pin("bidirectional", 180, 12.7, 48.26 - pair * 12.7 - 7.62, f"SERDES_RXN{pair}", str(pn))); pn += 1
    pin_list.append(_pin("input", 180, 12.7, 22.86, "REFCLK_P", str(pn))); pn += 1
    pin_list.append(_pin("input", 180, 12.7, 20.32, "REFCLK_N", str(pn))); pn += 1
    for i in range(8):
        pin_list.append(_pin("power_in", 0, -12.7, 48.26 - i * 5.08, f"+12V", str(pn))); pn += 1
    for i in range(16):
        pin_list.append(_pin("power_in", 0, -12.7, -2.54 - i * 2.54, "GND", str(pn))); pn += 1
    for i in range(6):
        pin_list.append(_pin("passive", 0, -12.7, -43.18 - i * 2.54, f"RESERVED{i+1}", str(pn))); pn += 1
    pins = "\n".join(pin_list)
    return f"""    (symbol "Connector:SERDES_40P" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 53.34 0) (effects (font (size 1.27 1.27))))
      (property "Value" "SERDES_40P" (at 0 -58.42 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_Samtec:SEAF-40" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "SERDES_40P_0_1"
        (rectangle (start -10.16 50.8) (end 10.16 -55.88) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "SERDES_40P_1_1"
{pins}
      )
    )"""

def lib_symbol_pcie_edge_x16():
    pin_list = []
    pn = 1
    for lane in range(16):
        y = 50.8 - lane * 2.54
        pin_list.append(_pin("bidirectional", 180, 17.78, y, f"TXP{lane}", str(pn))); pn += 1
        pin_list.append(_pin("bidirectional", 180, 17.78, y - 1.27, f"TXN{lane}", str(pn))); pn += 1
    for lane in range(16):
        y = 8.89 - lane * 2.54
        pin_list.append(_pin("bidirectional", 0, -17.78, y, f"RXP{lane}", str(pn))); pn += 1
        pin_list.append(_pin("bidirectional", 0, -17.78, y - 1.27, f"RXN{lane}", str(pn))); pn += 1
    pin_list.append(_pin("input", 0, -17.78, -33.02, "REFCLK_P", str(pn))); pn += 1
    pin_list.append(_pin("input", 0, -17.78, -35.56, "REFCLK_N", str(pn))); pn += 1
    pin_list.append(_pin("input", 0, -17.78, -38.1, "PERST_n", str(pn))); pn += 1
    pin_list.append(_pin("input", 0, -17.78, -40.64, "WAKE_n", str(pn))); pn += 1
    for i in range(4):
        pin_list.append(_pin("power_in", 0, -17.78, -43.18 - i * 2.54, "+12V", str(pn))); pn += 1
    for i in range(2):
        pin_list.append(_pin("power_in", 0, -17.78, -53.34 - i * 2.54, "+3.3V", str(pn))); pn += 1
    for i in range(8):
        pin_list.append(_pin("power_in", 0, -17.78, -58.42 - i * 2.54, "GND", str(pn))); pn += 1
    pins = "\n".join(pin_list)
    return f"""    (symbol "Connector_PCIe:PCIe_x16_Edge" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 55.88 0) (effects (font (size 1.27 1.27))))
      (property "Value" "PCIe_x16_Edge" (at 0 -76.2 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_PCIe:PCIe_x16_EdgeFinger" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "PCIe_x16_Edge_0_1"
        (rectangle (start -15.24 53.34) (end 15.24 -73.66) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "PCIe_x16_Edge_1_1"
{pins}
      )
    )"""

def lib_symbol_power_conn():
    pins = "\n".join([
        _pin("power_out", 180, 7.62, 2.54, "+12V", "1"),
        _pin("power_out", 180, 7.62, -2.54, "GND", "2"),
    ])
    return f"""    (symbol "Connector:Power_Conn" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Power_Conn" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector:Power_Conn_XT60" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Power_Conn_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Power_Conn_1_1"
{pins}
      )
    )"""

def lib_symbol_polyfuse():
    pins = "\n".join([
        _pin("passive", 0, -5.08, 0, "1", "1"),
        _pin("passive", 180, 5.08, 0, "2", "2"),
    ])
    return f"""    (symbol "Device:Polyfuse" (pin_numbers hide) (pin_names (offset 0)) (in_bom yes) (on_board yes)
      (property "Reference" "F" (at 0 2.54 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Polyfuse" (at 0 -2.54 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Fuse:Fuse_2920_7451Metric" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Polyfuse_0_1"
        (rectangle (start -2.54 1.016) (end 2.54 -1.016) (stroke (width 0.254) (type default)) (fill (type none)))
      )
      (symbol "Polyfuse_1_1"
{pins}
      )
    )"""

# == PCB Helpers ==

def pcb_header(board_w, board_h, layers=4):
    u = nuuid()
    if layers == 6:
        layer_defs = """    (0 "F.Cu" signal)
    (1 "In1.Cu" signal)
    (2 "In2.Cu" signal)
    (3 "In3.Cu" signal)
    (4 "In4.Cu" signal)
    (31 "B.Cu" signal)"""
    else:
        layer_defs = """    (0 "F.Cu" signal)
    (1 "In1.Cu" signal)
    (2 "In2.Cu" signal)
    (31 "B.Cu" signal)"""
    return f"""(kicad_pcb
  (version 20240108)
  (generator "hydra_v6_gen")
  (generator_version "6.0")
  (general
    (thickness 1.6)
    (legacy_teardrops no)
  )
  (paper "A4")
  (uuid "{u}")
  (layers
{layer_defs}
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
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user "B.Fabrication")
    (49 "F.Fab" user "F.Fabrication")
    (50 "User.1" user)
    (51 "User.2" user)
  )
  (setup
    (pad_to_mask_clearance 0)
    (allow_soldermask_bridges_in_footprints no)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (disableapertmacros no)
      (usegerberextensions no)
      (usegerberattributes yes)
      (usegerberadvancedattributes yes)
      (creategerberjobfile yes)
      (dashed_line_dash_ratio 12.000000)
      (dashed_line_gap_ratio 3.000000)
      (svgprecision 4)
      (plotframeref no)
      (viasonmask no)
      (mode 1)
      (useauxorigin no)
      (hpglpennumber 1)
      (hpglpenspeed 20)
      (hpglpendiameter 15.000000)
      (pdf_front_fp_property_popups yes)
      (pdf_back_fp_property_popups yes)
      (dxfpolygonmode yes)
      (dxfimperialunits yes)
      (dxfusepcbnewfont yes)
      (psnegative no)
      (psa4output no)
      (plotreference yes)
      (plotvalue yes)
      (plotfptext yes)
      (plotinvisibletext no)
      (sketchpadsonfab no)
      (subtractmaskfromsilk no)
      (outputformat 1)
      (mirror no)
      (drillshape 1)
      (scaleselection 1)
      (outputdirectory "")
    )
  )
  (net 0 "")
"""

def pcb_board_outline(w, h, r=1.0):
    lines = []
    lines.append(f'  (gr_line (start {r} 0) (end {w-r} 0) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_line (start {w} {r}) (end {w} {h-r}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_line (start {w-r} {h}) (end {r} {h}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_line (start 0 {h-r}) (end 0 {r}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_arc (start {r} {r}) (mid {0.293*r} {0.293*r}) (end 0 {r}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_arc (start {w-r} {r}) (mid {w-0.293*r} {0.293*r}) (end {w} {r}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_arc (start {w} {h-r}) (mid {w-0.293*r} {h-0.293*r}) (end {w-r} {h}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    lines.append(f'  (gr_arc (start {r} {h}) (mid {0.293*r} {h-0.293*r}) (end 0 {h-r}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{nuuid()}"))')
    return "\n".join(lines) + "\n"

def pcb_mounting_hole(x, y, size_mm=3.2):
    u1, u2, u3 = nuuid(), nuuid(), nuuid()
    pad_size = size_mm + 0.6
    return f"""  (footprint "MountingHole:MountingHole_{size_mm:.1f}mm" (layer "F.Cu")
    (at {x} {y})
    (uuid "{u1}")
    (property "Reference" "H" (at 0 -{pad_size} 0) (layer "F.SilkS") (uuid "{u2}")
      (effects (font (size 1 1) (thickness 0.15)))
    )
    (property "Value" "MountingHole" (at 0 {pad_size} 0) (layer "F.Fab") (uuid "{u3}")
      (effects (font (size 1 1) (thickness 0.15)))
    )
    (pad "" np_thru_hole circle (at 0 0) (size {size_mm} {size_mm}) (drill {size_mm}) (layers "*.Cu" "*.Mask"))
  )
"""

def pcb_footprint(fp_lib, ref, value, x, y, angle=0):
    u1, u2, u3 = nuuid(), nuuid(), nuuid()
    return f"""  (footprint "{fp_lib}" (layer "F.Cu")
    (at {x} {y} {angle})
    (uuid "{u1}")
    (property "Reference" "{ref}" (at 0 -2 0) (layer "F.SilkS") (uuid "{u2}")
      (effects (font (size 1 1) (thickness 0.15)))
    )
    (property "Value" "{value}" (at 0 2 0) (layer "F.Fab") (uuid "{u3}")
      (effects (font (size 1 1) (thickness 0.15)))
    )
  )
"""

def pcb_text(text, x, y, layer="F.SilkS", size=1.5):
    return f'  (gr_text "{text}" (at {x} {y}) (layer "{layer}") (uuid "{nuuid()}")\n    (effects (font (size {size} {size}) (thickness 0.15)))\n  )\n'

# ============================================================
# MODELS TABLE
# ============================================================

MODELS = {
    "ti180": {
        "lib_id": "FPGA_Efinix:Ti180",
        "lib_symbol_fn": lib_symbol_ti180,
        "footprint": "Package_BGA:BGA-484_1.0mm_22x22_23.0x23.0mm",
    },
    "lpddr4": {
        "lib_id": "Memory_DDR:LPDDR4_1GB",
        "lib_symbol_fn": lib_symbol_lpddr4,
        "footprint": "Package_BGA:BGA-200_0.8mm_14x14_12.0x10.0mm",
    },
    "w25q128": {
        "lib_id": "Memory_Flash:W25Q128JV",
        "lib_symbol_fn": lib_symbol_w25q128,
        "footprint": "Package_SO:SOP-8_3.9x4.9mm_P1.27mm",
    },
    "arc_a380": {
        "lib_id": "GPU:Arc_A380",
        "lib_symbol_fn": lib_symbol_arc_a380,
        "footprint": "Package_BGA:BGA-2660_45x37.5mm",
    },
    "pcie_switch_4port": {
        "lib_id": "Interface_PCIe:PI7C9X2G404SL",
        "lib_symbol_fn": lib_symbol_pcie_switch_4port,
        "footprint": "Package_BGA:BGA-176_15x15mm",
    },
    "ir3842": {
        "lib_id": "Regulator_Switching:IR3842",
        "lib_symbol_fn": lib_symbol_ir3842,
        "footprint": "Package_DFN_QFN:QFN-16-1EP_5x5mm_P0.65mm_EP3.35x3.35mm",
    },
    "tlv62130": {
        "lib_id": "Regulator_Switching:TLV62130",
        "lib_symbol_fn": lib_symbol_tlv62130,
        "footprint": "Package_DFN_QFN:QFN-16-1EP_3x3mm_P0.5mm_EP1.68x1.68mm",
    },
    "ap2112k": {
        "lib_id": "Regulator_Linear:AP2112K",
        "lib_symbol_fn": lib_symbol_ap2112k,
        "footprint": "Package_TO_SOT_SMD:SOT-23-5",
    },
    "osc_diff": {
        "lib_id": "Oscillator:ASFL1_100MHz",
        "lib_symbol_fn": lib_symbol_osc_diff,
        "footprint": "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm",
    },
    "serdes_conn": {
        "lib_id": "Connector:SERDES_40P",
        "lib_symbol_fn": lib_symbol_serdes_conn,
        "footprint": "Connector_Samtec:SEAF-40",
    },
    "pcie_edge_x16": {
        "lib_id": "Connector_PCIe:PCIe_x16_Edge",
        "lib_symbol_fn": lib_symbol_pcie_edge_x16,
        "footprint": "Connector_PCIe:PCIe_x16_EdgeFinger",
    },
    "power_conn": {
        "lib_id": "Connector:Power_Conn",
        "lib_symbol_fn": lib_symbol_power_conn,
        "footprint": "Connector:Power_Conn_XT60",
    },
    "polyfuse": {
        "lib_id": "Device:Polyfuse",
        "lib_symbol_fn": lib_symbol_polyfuse,
        "footprint": "Fuse:Fuse_2920_7451Metric",
    },
    "resistor": {
        "lib_id": "Device:R",
        "lib_symbol_fn": lib_symbol_resistor,
        "footprint": "Resistor_SMD:R_0402_1005Metric",
    },
    "cap": {
        "lib_id": "Device:C",
        "lib_symbol_fn": lib_symbol_cap,
        "footprint": "Capacitor_SMD:C_0402_1005Metric",
    },
    "cap_0805": {
        "lib_id": "Device:C_0805",
        "lib_symbol_fn": lib_symbol_cap_0805,
        "footprint": "Capacitor_SMD:C_0805_2012Metric",
    },
    "cap_1206": {
        "lib_id": "Device:C_1206",
        "lib_symbol_fn": lib_symbol_cap_1206,
        "footprint": "Capacitor_SMD:C_1206_3216Metric",
    },
    "led": {
        "lib_id": "Device:LED",
        "lib_symbol_fn": lib_symbol_led,
        "footprint": "LED_SMD:LED_0603_1608Metric",
    },
    "inductor": {
        "lib_id": "Device:L",
        "lib_symbol_fn": lib_symbol_inductor,
        "footprint": "Inductor_SMD:L_1210_3225Metric",
    },
}

# ============================================================
# SPLICE HANDLERS (jump table)
# ============================================================

def _handle_text(ctx, sp):
    ctx["s"] += place_text(sp["text"], sp["x"], sp["y"], sp.get("size", 2.54))

def _handle_symbol(ctx, sp):
    model = sp.get("model")
    lib_id = MODELS[model]["lib_id"] if model else sp["lib_id"]
    ref = sp["ref"]
    if ref.startswith("C@"):
        ref = f"C{ctx['c_idx']}"
        ctx["c_idx"] += 1
    elif ref.startswith("R@"):
        ref = f"R{ctx['r_idx']}"
        ctx["r_idx"] += 1
    elif ref.startswith("D@"):
        ref = f"D{ctx['d_idx']}"
        ctx["d_idx"] += 1
    ctx["s"] += place_symbol(lib_id, ref, sp["value"], sp["x"], sp["y"],
                              sp.get("angle", 0), sp.get("unit", 1), sp.get("extra_props"))

def _handle_bus_labels(ctx, sp):
    signals = sp["signals"]
    prefix = sp["prefix"]
    ax, ay_base, ay_step = sp["a_x"], sp["a_y_base"], sp.get("a_y_step", -2.54)
    bx, by_base, by_step = sp["b_x"], sp["b_y_base"], sp.get("b_y_step", -2.54)
    for j, sig in enumerate(signals):
        label_name = f"{prefix}{sig}"
        ctx["s"] += place_label(label_name, ax, ay_base + j * ay_step)
        ctx["s"] += place_label(label_name, bx, by_base + j * by_step)

def _handle_bus_remap_labels(ctx, sp):
    signals = sp["signals"]
    prefix = sp["prefix"]
    ax, ay_base = sp["a_x"], sp["a_y_base"]
    a_offset = sp["a_offset"]
    bx, by_base = sp["b_x"], sp["b_y_base"]
    remap = sp["remap"]
    remap_order = sp["remap_order"]
    for j, sig in enumerate(signals):
        label_name = f"{prefix}{sig}"
        ctx["s"] += place_label(label_name, ax, ay_base + (a_offset + j) * -2.54)
        ctrl_idx = 16 + 6 + remap_order.index(remap[sig])
        ctx["s"] += place_label(label_name, bx, by_base - ctrl_idx * 2.54)

def _handle_global_labels(ctx, sp):
    for item in sp["labels"]:
        ctx["s"] += place_global_label(item[0], item[1], item[2], item[3], item[4])

def _handle_global_label_fan(ctx, sp):
    signals = sp["signals"]
    x, y_base, y_step = sp["x"], sp["y_base"], sp.get("y_step", -2.54)
    angle = sp.get("angle", 0)
    shape = sp.get("shape", "bidirectional")
    for j, sig in enumerate(signals):
        ctx["s"] += place_global_label(sig, x, y_base + j * y_step, angle, shape)

def _handle_wire(ctx, sp):
    ctx["s"] += place_wire(sp["x1"], sp["y1"], sp["x2"], sp["y2"])

def _handle_decoupling(ctx, sp):
    x_base, y_c, count = sp["x_base"], sp["y"], sp.get("count", 2)
    x_step = sp.get("x_step", 7)
    vcc_label, vcc_shape = sp["vcc_label"], sp.get("vcc_shape", "input")
    y_vcc_off = sp.get("y_vcc_off", 2.54)
    y_gnd_off = sp.get("y_gnd_off", -2.54)
    for dc in range(count):
        x = x_base + dc * x_step
        ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF", x, y_c)
        ctx["s"] += place_global_label(vcc_label, x, y_c + y_vcc_off, 0, vcc_shape)
        ctx["s"] += place_global_label("GND", x, y_c + y_gnd_off, 0, "input")
        ctx["c_idx"] += 1
    if sp.get("bulk"):
        bx = sp["bulk_x"]
        by = sp["bulk_y"]
        ctx["s"] += place_symbol("Device:C_0805", f"C{ctx['c_idx']}", "10uF", bx, by)
        ctx["s"] += place_global_label(vcc_label, bx, by + y_vcc_off, 0, vcc_shape)
        ctx["s"] += place_global_label("GND", bx, by + y_gnd_off, 0, "input")
        ctx["c_idx"] += 1

def _handle_pull_ups(ctx, sp):
    signals = sp["signals"]
    x_base, y, x_step = sp["x_base"], sp["y"], sp.get("x_step", 7)
    vcc_label = sp["vcc_label"]
    label_prefix = sp["label_prefix"]
    for j, sig in enumerate(signals):
        x = x_base + j * x_step
        ctx["s"] += place_symbol("Device:R", f"R{ctx['r_idx']}", sp.get("value", "10k"), x, y)
        ctx["s"] += place_global_label(vcc_label, x, y + 2.54, 0, "input")
        ctx["s"] += place_label(f"{label_prefix}{sig}", x, y - 2.54)
        ctx["r_idx"] += 1

def _handle_gpu_array(ctx, sp):
    positions = sp["positions"]
    refs = sp["refs"]
    for i, ((gx, gy), ref) in enumerate(zip(positions, refs)):
        ctx["s"] += place_symbol("GPU:Arc_A380", ref, f"Arc A380 #{i+1}", gx, gy)
        for j, suffix in enumerate(["TXP", "TXN", "RXP", "RXN"]):
            label_name = f"GPU{i}_PCIE_{suffix}"
            ctx["s"] += place_global_label(label_name, gx - 20, gy + 22.86 - j * 2.54, 180, "bidirectional")
        ctx["s"] += place_global_label(f"VCC_GPU{i}", gx + 20, gy + 22.86, 0, "input")
        ctx["s"] += place_global_label(f"VCC_GPU{i}", gx + 20, gy + 20.32, 0, "input")
        ctx["s"] += place_global_label("GND", gx + 20, gy - 22.86, 0, "input")
        for dc in range(2):
            ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF", gx + 25 + dc * 7, gy)
            ctx["s"] += place_global_label(f"VCC_GPU{i}", gx + 25 + dc * 7, gy + 2.54, 0, "input")
            ctx["s"] += place_global_label("GND", gx + 25 + dc * 7, gy - 2.54, 0, "input")
            ctx["c_idx"] += 1
        ctx["s"] += place_symbol("Device:C_0805", f"C{ctx['c_idx']}", "10uF", gx + 25, gy + 10)
        ctx["s"] += place_global_label(f"VCC_GPU{i}", gx + 25, gy + 10 + 2.54, 0, "input")
        ctx["s"] += place_global_label("GND", gx + 25, gy + 10 - 2.54, 0, "input")
        ctx["c_idx"] += 1

def _handle_ir3842_array(ctx, sp):
    positions = sp["positions"]
    refs = sp["refs"]
    inductor_refs = sp["inductor_refs"]
    for i, ((px, py), uref, lref) in enumerate(zip(positions, refs, inductor_refs)):
        ctx["s"] += place_symbol("Regulator_Switching:IR3842", uref, f"IR3842 GPU{i} 25A", px, py)
        ctx["s"] += place_global_label("VCC_12V", px - 15, py + 7.62, 180, "input")
        ctx["s"] += place_global_label(f"VCC_GPU{i}", px + 15, py + 7.62, 0, "output")
        ctx["s"] += place_global_label("GND", px + 15, py + 0, 0, "input")
        ctx["s"] += place_global_label("GND", px + 15, py - 2.54, 0, "input")
        ctx["s"] += place_symbol("Device:L", lref, "4.7uH", px + 20, py + 2.54)
        ctx["s"] += place_wire(px + 12.7, py + 2.54, px + 20, py + 2.54)
        ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF", px - 20, py)
        ctx["s"] += place_global_label("VCC_12V", px - 20, py + 2.54, 0, "input")
        ctx["s"] += place_global_label("GND", px - 20, py - 2.54, 0, "input")
        ctx["c_idx"] += 1

def _handle_regulator_block(ctx, sp):
    lib_id = MODELS[sp["model"]]["lib_id"]
    ref, value = sp["ref"], sp["value"]
    x, y = sp["x"], sp["y"]
    ctx["s"] += place_symbol(lib_id, ref, value, x, y)
    for gl in sp["globals"]:
        ctx["s"] += place_global_label(gl[0], gl[1], gl[2], gl[3], gl[4])
    for cap_def in sp.get("caps", []):
        if cap_def[0] == "cap":
            ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF", cap_def[1], cap_def[2])
            ctx["c_idx"] += 1
        elif cap_def[0] == "cap_0805":
            ctx["s"] += place_symbol("Device:C_0805", f"C{ctx['c_idx']}", "10uF", cap_def[1], cap_def[2])
            ctx["c_idx"] += 1
        for gl in cap_def[3:]:
            ctx["s"] += place_global_label(gl[0], gl[1], gl[2], gl[3], gl[4])

def _handle_bulk_caps(ctx, sp):
    count = sp["count"]
    model = sp.get("model", "cap_0805")
    lib_id = MODELS[model]["lib_id"]
    cap_value = sp.get("value", "10uF")
    x_base, y = sp["x_base"], sp["y"]
    x_step = sp.get("x_step", 15)
    vcc_label = sp["vcc_label"]
    for bc in range(count):
        ctx["s"] += place_symbol(lib_id, f"C{ctx['c_idx']}", cap_value, x_base + bc * x_step, y)
        ctx["s"] += place_global_label(vcc_label, x_base + bc * x_step, y + 2.54, 0, "input")
        ctx["s"] += place_global_label("GND", x_base + bc * x_step, y - 2.54, 0, "input")
        ctx["c_idx"] += 1

def _handle_fill_caps(ctx, sp):
    lib_id = MODELS[sp["model"]]["lib_id"]
    cap_value = sp["value"]
    limit = sp["limit"]
    cols = sp["cols"]
    x_base, y_base = sp["x_base"], sp["y_base"]
    x_step, y_step = sp["x_step"], sp["y_step"]
    start_offset = sp.get("start_offset", lambda idx: ((idx - 1) % cols, (idx - 1) // cols))
    while ctx["c_idx"] <= limit:
        col, row = start_offset(ctx["c_idx"])
        ctx["s"] += place_symbol(lib_id, f"C{ctx['c_idx']}", cap_value, x_base + col * x_step, y_base + row * y_step)
        ctx["c_idx"] += 1

def _handle_switch_downstream(ctx, sp):
    x, y_base = sp["x"], sp["y_base"]
    for port in range(4):
        for j, suffix in enumerate(["TXP", "TXN", "RXP", "RXN"]):
            label_name = f"GPU{port}_PCIE_{suffix}"
            ctx["s"] += place_global_label(label_name, x, y_base - port * 12.7 - j * 2.54, 0, "bidirectional")

def _handle_serdes_conn_labels(ctx, sp):
    x, y_base = sp["x"], sp["y_base"]
    signals = sp["signals"]
    for sig_name, y_off in signals:
        ctx["s"] += place_global_label(sig_name, x, y_base + y_off, 0, "bidirectional")
    for gl in sp.get("extra_globals", []):
        ctx["s"] += place_global_label(gl[0], gl[1], gl[2], gl[3], gl[4])

def _handle_bp_slot_column(ctx, sp):
    positions = sp["positions"]
    start_idx = sp["start_idx"]
    for i, (jx, jy) in enumerate(positions):
        slot_num = start_idx + i
        ctx["s"] += place_symbol("Connector:SERDES_40P", f"J{slot_num}", f"Node Slot {slot_num}", jx, jy)
        ctx["s"] += place_global_label("VCC_12V", jx - 15, jy + 48.26, 180, "input")
        ctx["s"] += place_global_label("GND", jx - 15, jy - 2.54, 180, "input")
        ctx["s"] += place_global_label(f"SLOT{slot_num}_CLK_P", jx + 15, jy + 22.86, 0, "input")
        ctx["s"] += place_global_label(f"SLOT{slot_num}_CLK_N", jx + 15, jy + 20.32, 0, "input")

def _handle_mesh_wiring(ctx, sp):
    mesh_pairs = sp["mesh_pairs"]
    all_positions = sp["all_positions"]
    for (sa, sb) in mesh_pairs:
        ax, ay = all_positions[sa - 1]
        bx, by = all_positions[sb - 1]
        for j, (tx_suffix, _rx_suffix) in enumerate([("TXP0", "RXP0"), ("TXN0", "RXN0")]):
            label_name = f"MESH_{sa}_{sb}_{tx_suffix}"
            ctx["s"] += place_global_label(label_name, ax + 15, ay + 48.26 - j * 2.54, 0, "bidirectional")
            ctx["s"] += place_global_label(label_name, bx + 15, by + 43.18 - j * 2.54, 0, "bidirectional")

def _handle_clock_fan(ctx, sp):
    x_base, y_base = sp["x_base"], sp["y_base"]
    count = sp["count"]
    for i in range(count):
        ctx["s"] += place_global_label(f"SLOT{i+1}_CLK_P", x_base + (i % 4) * 10, y_base + 2.54 + (i // 4) * 10, 0, "output")
        ctx["s"] += place_global_label(f"SLOT{i+1}_CLK_N", x_base + (i % 4) * 10, y_base - 2.54 + (i // 4) * 10, 0, "output")

def _handle_led_block(ctx, sp):
    names = sp["names"]
    lx, ly_base, ly_step = sp["lx"], sp["ly_base"], sp.get("ly_step", 12)
    for i, name in enumerate(names):
        ly = ly_base + i * ly_step
        ctx["s"] += place_symbol("Device:LED", f"D{ctx['d_idx']}", name, lx + 10, ly)
        ctx["s"] += place_symbol("Device:R", f"R{ctx['r_idx']}", "330", lx, ly)
        ctx["s"] += place_global_label("VCC_3V3", lx, ly + 2.54, 0, "input")
        ctx["s"] += place_global_label("GND", lx + 10 + 3.81, ly, 0, "input")
        ctx["d_idx"] += 1
        ctx["r_idx"] += 1

def _handle_node_led_pair(ctx, sp):
    # Exactly reproduces the node LED sequence: D, R, global, wire, global
    led_ref, led_value = sp["led_ref"], sp["led_value"]
    r_ref, r_value = sp["r_ref"], sp["r_value"]
    lx, ly = sp["lx"], sp["ly"]
    rx, ry = sp["rx"], sp["ry"]
    ctx["s"] += place_symbol("Device:LED", led_ref, led_value, lx, ly)
    ctx["s"] += place_symbol("Device:R", r_ref, r_value, rx, ry)
    ctx["s"] += place_global_label("VCC_3V3", rx, ry + 2.54, 0, "input")
    ctx["s"] += place_wire(rx, ry - 2.54, lx - 3.81, ly)
    ctx["s"] += place_global_label("GND", lx + 3.81, ly, 0, "input")

def _handle_bp_decoupling_grid(ctx, sp):
    count = sp["count"]
    cols = sp["cols"]
    x_base, y_base = sp["x_base"], sp["y_base"]
    x_step, y_step = sp["x_step"], sp["y_step"]
    vcc_label = sp["vcc_label"]
    for i in range(count):
        col = i % cols
        row = i // cols
        x = x_base + col * x_step
        y = y_base + row * y_step
        ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF", x, y)
        ctx["s"] += place_global_label(vcc_label, x, y + 2.54, 0, "input")
        ctx["s"] += place_global_label("GND", x, y - 2.54, 0, "input")
        ctx["c_idx"] += 1

SPLICE_HANDLERS = {
    "text": _handle_text,
    "symbol": _handle_symbol,
    "bus_labels": _handle_bus_labels,
    "bus_remap_labels": _handle_bus_remap_labels,
    "global_labels": _handle_global_labels,
    "global_label_fan": _handle_global_label_fan,
    "wire": _handle_wire,
    "decoupling": _handle_decoupling,
    "pull_ups": _handle_pull_ups,
    "gpu_array": _handle_gpu_array,
    "ir3842_array": _handle_ir3842_array,
    "regulator_block": _handle_regulator_block,
    "bulk_caps": _handle_bulk_caps,
    "fill_caps": _handle_fill_caps,
    "switch_downstream": _handle_switch_downstream,
    "serdes_conn_labels": _handle_serdes_conn_labels,
    "bp_slot_column": _handle_bp_slot_column,
    "mesh_wiring": _handle_mesh_wiring,
    "clock_fan": _handle_clock_fan,
    "led_block": _handle_led_block,
    "node_led_pair": _handle_node_led_pair,
    "bp_decoupling_grid": _handle_bp_decoupling_grid,
}

# ============================================================
# TWO-PASS ENGINE
# ============================================================

def run_sch_engine(board_def, output_dir, filename):
    reset_uuids()
    gen_project(board_def["project_name"], output_dir)

    hdr = board_def["header"]
    s = sch_header(hdr["paper"], hdr["title"], hdr["rev"])
    s += "  (lib_symbols\n"
    for model_key in board_def["lib_symbol_order"]:
        s += MODELS[model_key]["lib_symbol_fn"]() + "\n"
    s += "  )\n"

    ctx = {"s": s, "c_idx": 1, "r_idx": 1, "d_idx": 1}

    for sp in board_def["splices"]:
        SPLICE_HANDLERS[sp["type"]](ctx, sp)

    ctx["s"] += sch_footer()

    path = os.path.join(output_dir, filename)
    with open(path, "w") as f:
        f.write(ctx["s"])
    print(f"  wrote {path}")

def run_pcb_engine(pcb_def, output_dir, filename):
    W, H = pcb_def["width"], pcb_def["height"]
    s = pcb_header(W, H, layers=pcb_def.get("layers", 4))
    s += pcb_board_outline(W, H)
    s += pcb_text(pcb_def["title_text"], pcb_def["title_x"], pcb_def["title_y"],
                  pcb_def.get("title_layer", "F.SilkS"), pcb_def.get("title_size", 2.0))

    for mx, my in pcb_def["mounting_holes"]:
        s += pcb_mounting_hole(mx, my, pcb_def.get("mounting_hole_size", 3.2))

    for fp in pcb_def["footprints"]:
        s += pcb_footprint(fp[0], fp[1], fp[2], fp[3], fp[4], fp[5] if len(fp) > 5 else 0)

    ctx = {"s": s, "c_idx": 1}
    for step in pcb_def.get("cap_steps", []):
        step["fn"](step, ctx)
    s = ctx["s"]
    s += ")\n"

    path = os.path.join(output_dir, filename)
    with open(path, "w") as f:
        f.write(s)
    print(f"  wrote {path}")

# ============================================================
# NODE PCB CAP STEPS
# ============================================================

def _node_pcb_ic_caps(step, ctx):
    ic_positions = step["ic_positions"]
    for ix, iy in ic_positions:
        for dc in range(2):
            ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                       f"C{ctx['c_idx']}", "100nF", ix - 8 + dc * 3, iy + 12)
            ctx["c_idx"] += 1

def _node_pcb_fill_100nf(step, ctx):
    while ctx["c_idx"] <= 60:
        col = (ctx["c_idx"] - 25) % 15
        row = (ctx["c_idx"] - 25) // 15
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                   f"C{ctx['c_idx']}", "100nF", 10 + col * 10, 85 + row * 3)
        ctx["c_idx"] += 1

def _node_pcb_bulk_caps(step, ctx):
    while ctx["c_idx"] <= 80:
        col = (ctx["c_idx"] - 61) % 10
        row = (ctx["c_idx"] - 61) // 10
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0805_2012Metric",
                                   f"C{ctx['c_idx']}", "10uF", 15 + col * 15, 92 + row * 3)
        ctx["c_idx"] += 1

# ============================================================
# BP PCB CAP STEPS
# ============================================================

def _bp_pcb_slot_caps(step, ctx):
    slot_x_positions = step["slot_x_positions"]
    for _i, sx in enumerate(slot_x_positions):
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                   f"C{ctx['c_idx']}", "100nF", sx, 45)
        ctx["c_idx"] += 1
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                   f"C{ctx['c_idx']}", "100nF", sx, 75)
        ctx["c_idx"] += 1

def _bp_pcb_bulk_caps(step, ctx):
    for i in range(8):
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_1206_3216Metric",
                                   f"C{ctx['c_idx']}", "47uF", 30 + i * 33, 100)
        ctx["c_idx"] += 1

# ============================================================
# BOARD DEFINITIONS
# ============================================================

_PCIE_LABELS_18 = ["PCIE_TXP0","PCIE_TXN0","PCIE_RXP0","PCIE_RXN0",
                    "PCIE_TXP1","PCIE_TXN1","PCIE_RXP1","PCIE_RXN1",
                    "PCIE_TXP2","PCIE_TXN2","PCIE_RXP2","PCIE_RXN2",
                    "PCIE_TXP3","PCIE_TXN3","PCIE_RXP3","PCIE_RXN3",
                    "PCIE_REFCLK_P","PCIE_REFCLK_N"]

_DDR_DATA_SIGNALS = ["DQ0","DQ1","DQ2","DQ3","DQ4","DQ5","DQ6","DQ7",
                     "DQ8","DQ9","DQ10","DQ11","DQ12","DQ13","DQ14","DQ15",
                     "CA0","CA1","CA2","CA3","CA4","CA5"]

_SERDES_8_SIGNALS = [
    ("SERDES_TXP0", 48.26), ("SERDES_TXN0", 45.72),
    ("SERDES_RXP0", 43.18), ("SERDES_RXN0", 40.64),
    ("SERDES_TXP1", 35.56), ("SERDES_TXN1", 33.02),
    ("SERDES_RXP1", 30.48), ("SERDES_RXN1", 27.94),
]

NODE_BOARD = {
    "project_name": "hydra_node",
    "header": {"paper": "A0", "title": "Hydra Mesh v6.0 -- Ti180 FPGA GPU Compute Node", "rev": "6.0"},
    "lib_symbol_order": [
        "ti180", "lpddr4", "w25q128", "arc_a380", "pcie_switch_4port",
        "ir3842", "tlv62130", "ap2112k", "osc_diff", "serdes_conn",
        "resistor", "cap", "cap_0805", "led", "inductor",
    ],
    "splices": [
        {"type": "text", "text": "HYDRA NODE -- Ti180 + 4x Arc A380 GPU", "x": 200, "y": 10, "size": 3.0},
        # Section 1: Ti180 + LPDDR4 + Flash
        {"type": "text", "text": "Ti180 FPGA + Memory + Flash", "x": 80, "y": 45, "size": 2.0},
        {"type": "symbol", "model": "ti180", "ref": "U1", "value": "Ti180 J484", "x": 80, "y": 80},
        {"type": "symbol", "model": "lpddr4", "ref": "U2", "value": "LPDDR4 1GB", "x": 80, "y": 140},
        {"type": "symbol", "model": "w25q128", "ref": "U3", "value": "W25Q128JV 16MB", "x": 130, "y": 80},
        # DDR bus labels
        {"type": "bus_labels", "prefix": "DDR_", "signals": _DDR_DATA_SIGNALS,
         "a_x": 50, "a_y_base": 135.88, "a_y_step": -2.54,
         "b_x": 62, "b_y_base": 173.02, "b_y_step": -2.54},
        # DDR control remap labels
        {"type": "bus_remap_labels", "prefix": "DDR_", "signals": ["CK","CKE","CS","ODT","RESET"],
         "a_x": 50, "a_y_base": 135.88, "a_offset": 22,
         "b_x": 62, "b_y_base": 173.02,
         "remap": {"CK": "CK_t", "CKE": "CKE", "CS": "CS", "ODT": "ODT", "RESET": "RESET_n"},
         "remap_order": ["CK_t","CK_c","CS","CKE","ODT","RESET_n"]},
        # SPI labels
        {"type": "bus_labels", "prefix": "FLASH_", "signals": ["SCK","MOSI","MISO","CS"],
         "a_x": 50, "a_y_base": 64.76, "a_y_step": -2.54,
         "b_x": 117, "b_y_base": 83.81, "b_y_step": -2.54},
        # Ti180 power
        {"type": "global_labels", "labels": [
            ("VCC_0V9", 110, 29.2, 0, "input"),
            ("VCC_1V8", 110, 26.66, 0, "input"),
            ("VCC_3V3", 110, 24.12, 0, "input"),
            ("GND", 110, 21.58, 0, "input"),
            ("GND", 98, 134.92, 0, "input"),
            ("VCC_3V3", 143, 76.19, 0, "input"),
            ("GND", 117, 76.19, 0, "input"),
        ]},
        # Ti180 PCIe global labels
        {"type": "global_label_fan", "signals": _PCIE_LABELS_18,
         "x": 110, "y_base": 113.02, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # Ti180 SERDES global labels
        {"type": "global_label_fan", "signals": [
            "SERDES_TXP0","SERDES_TXN0","SERDES_RXP0","SERDES_RXN0",
            "SERDES_TXP1","SERDES_TXN1","SERDES_RXP1","SERDES_RXN1"],
         "x": 110, "y_base": 135.88, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # Decoupling near Ti180
        {"type": "decoupling", "x_base": 120, "y": 90, "count": 2, "x_step": 7,
         "vcc_label": "VCC_0V9", "bulk": True, "bulk_x": 120, "bulk_y": 100},
        # JTAG pull-ups
        {"type": "pull_ups", "signals": ["TCK","TMS","TDI","PROGRAMN"],
         "x_base": 120, "y": 40, "x_step": 7, "vcc_label": "VCC_3V3", "label_prefix": "JTAG_", "value": "10k"},
        # Section 2: PCIe Switch
        {"type": "text", "text": "PCIe Switch", "x": 190, "y": 55, "size": 2.0},
        {"type": "symbol", "model": "pcie_switch_4port", "ref": "U12", "value": "PI7C9X2G404SL", "x": 190, "y": 90},
        # Upstream labels
        {"type": "global_label_fan", "signals": _PCIE_LABELS_18,
         "x": 165, "y_base": 133.18, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # Switch power
        {"type": "global_labels", "labels": [
            ("VCC_0V9", 165, 36.66, 180, "input"),
            ("VCC_3V3", 165, 34.12, 180, "input"),
            ("GND", 165, 31.58, 180, "input"),
        ]},
        # Switch downstream
        {"type": "switch_downstream", "x": 215, "y_base": 84.92},
        # Decoupling near switch
        {"type": "decoupling", "x_base": 220, "y": 100, "count": 2, "x_step": 7,
         "vcc_label": "VCC_0V9", "bulk": True, "bulk_x": 220, "bulk_y": 110},
        # Section 3: GPU Array
        {"type": "text", "text": "GPU Array -- 4x Intel Arc A380", "x": 300, "y": 10, "size": 2.0},
        {"type": "gpu_array",
         "positions": [(300, 40), (300, 100), (300, 160), (300, 220)],
         "refs": ["U4", "U5", "U6", "U7"]},
        # Section 4: Power
        {"type": "text", "text": "POWER SUPPLY", "x": 200, "y": 255, "size": 2.0},
        {"type": "ir3842_array",
         "positions": [(240, 280), (280, 280), (320, 280), (360, 280)],
         "refs": ["U13", "U14", "U15", "U16"],
         "inductor_refs": ["L1", "L2", "L3", "L4"]},
        # TLV62130 0.9V
        {"type": "regulator_block", "model": "tlv62130", "ref": "U17", "value": "TLV62130 0.9V",
         "x": 60, "y": 280,
         "globals": [
             ("VCC_12V", 47, 285.08, 180, "input"),
             ("VCC_0V9", 73, 277.46, 0, "output"),
             ("GND", 73, 282.54, 0, "input"),
         ],
         "caps": [
             ("cap", 80, 280, ("VCC_0V9", 80, 282.54, 0, "input"), ("GND", 80, 277.46, 0, "input")),
             ("cap_0805", 87, 280, ("VCC_0V9", 87, 282.54, 0, "input"), ("GND", 87, 277.46, 0, "input")),
         ]},
        # TLV62130 1.8V
        {"type": "regulator_block", "model": "tlv62130", "ref": "U18", "value": "TLV62130 1.8V",
         "x": 100, "y": 280,
         "globals": [
             ("VCC_12V", 87, 285.08, 180, "input"),
             ("VCC_1V8", 113, 277.46, 0, "output"),
             ("GND", 113, 282.54, 0, "input"),
         ],
         "caps": [
             ("cap", 120, 280, ("VCC_1V8", 120, 282.54, 0, "input"), ("GND", 120, 277.46, 0, "input")),
             ("cap_0805", 127, 280, ("VCC_1V8", 127, 282.54, 0, "input"), ("GND", 127, 277.46, 0, "input")),
         ]},
        # AP2112K 3.3V
        {"type": "regulator_block", "model": "ap2112k", "ref": "U19", "value": "AP2112K 3.3V",
         "x": 140, "y": 280,
         "globals": [
             ("VCC_12V", 127, 282.54, 180, "input"),
             ("GND", 127, 277.46, 180, "input"),
             ("VCC_3V3", 153, 282.54, 0, "output"),
         ],
         "caps": [
             ("cap", 160, 280, ("VCC_3V3", 160, 282.54, 0, "input"), ("GND", 160, 277.46, 0, "input")),
         ]},
        # L5 bulk input filter
        {"type": "symbol", "model": "inductor", "ref": "L5", "value": "4.7uH", "x": 180, "y": 280},
        {"type": "global_labels", "labels": [("VCC_12V", 180, 282.54, 0, "input")]},
        # Bulk caps near regulators
        {"type": "bulk_caps", "count": 4, "model": "cap_0805", "value": "10uF",
         "x_base": 60, "y": 300, "x_step": 15, "vcc_label": "VCC_12V"},
        # Section 5: Connector + misc
        {"type": "text", "text": "SERDES CONNECTOR + MISC", "x": 40, "y": 285, "size": 1.5},
        {"type": "symbol", "model": "serdes_conn", "ref": "J1", "value": "SERDES SEAF-40", "x": 40, "y": 300},
        {"type": "serdes_conn_labels", "x": 55, "y_base": 300,
         "signals": _SERDES_8_SIGNALS,
         "extra_globals": [
             ("VCC_12V", 25, 348.26, 180, "input"),
             ("GND", 25, 297.46, 180, "input"),
         ]},
        # Y1 oscillator
        {"type": "symbol", "model": "osc_diff", "ref": "Y1", "value": "100MHz Diff", "x": 160, "y": 60},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 150, 62.54, 180, "input"),
            ("GND", 150, 57.46, 180, "input"),
            ("PCIE_REFCLK_P", 170, 62.54, 0, "output"),
            ("PCIE_REFCLK_N", 170, 57.46, 0, "output"),
        ]},
        # Status LEDs
        {"type": "node_led_pair", "led_ref": "D1", "led_value": "PWR_GREEN", "lx": 20, "ly": 20,
         "r_ref": "R1", "r_value": "330", "rx": 10, "ry": 20},
        {"type": "node_led_pair", "led_ref": "D2", "led_value": "ACT_BLUE", "lx": 20, "ly": 32,
         "r_ref": "R2", "r_value": "330", "rx": 10, "ry": 32},
        # Fill 100nF caps to C60
        {"type": "fill_caps", "model": "cap", "value": "100nF", "limit": 60, "cols": 12,
         "x_base": 60, "y_base": 320, "x_step": 8, "y_step": 6,
         "start_offset": lambda idx: ((idx - 1) % 12, (idx - 1) // 12)},
        # Fill 10uF bulk caps C61-C80
        {"type": "fill_caps", "model": "cap_0805", "value": "10uF", "limit": 80, "cols": 10,
         "x_base": 200, "y_base": 320, "x_step": 10, "y_step": 6,
         "start_offset": lambda idx: ((idx - 61) % 10, (idx - 61) // 10)},
    ],
}

_BP_J14_POS = [(150, 40), (150, 100), (150, 160), (150, 220)]
_BP_J58_POS = [(250, 40), (250, 100), (250, 160), (250, 220)]

BACKPLANE_BOARD = {
    "project_name": "hydra_backplane",
    "header": {"paper": "A1", "title": "Hydra Mesh v6.0 -- Rack Backplane (8-slot SERDES)", "rev": "6.0"},
    "lib_symbol_order": [
        "serdes_conn", "pcie_edge_x16", "power_conn", "polyfuse", "osc_diff",
        "resistor", "cap", "cap_1206", "led",
    ],
    "splices": [
        {"type": "text", "text": "RACK BACKPLANE -- 8 Node Slots + PCIe x16", "x": 200, "y": 10, "size": 3.0},
        # J9 PCIe x16 edge
        {"type": "symbol", "model": "pcie_edge_x16", "ref": "J9", "value": "PCIe x16 Host", "x": 30, "y": 150},
        {"type": "global_labels", "labels": [
            ("VCC_12V", 10, 106.82, 180, "output"),
            ("VCC_3V3", 10, 96.66, 180, "output"),
            ("GND", 10, 91.58, 180, "input"),
        ]},
        # J1-J4
        {"type": "bp_slot_column", "positions": _BP_J14_POS, "start_idx": 1},
        # J5-J8
        {"type": "bp_slot_column", "positions": _BP_J58_POS, "start_idx": 5},
        # Mesh wiring
        {"type": "mesh_wiring",
         "mesh_pairs": [(1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 8)],
         "all_positions": _BP_J14_POS + _BP_J58_POS},
        # J10 power + F1 polyfuse
        {"type": "symbol", "model": "power_conn", "ref": "J10", "value": "12V Power In", "x": 300, "y": 30},
        {"type": "global_labels", "labels": [("GND", 310, 27.46, 0, "input")]},
        {"type": "wire", "x1": 307.62, "y1": 32.54, "x2": 290, "y2": 32.54},
        {"type": "symbol", "model": "polyfuse", "ref": "F1", "value": "15A Polyfuse", "x": 270, "y": 30},
        {"type": "wire", "x1": 275.08, "y1": 30, "x2": 290, "y2": 30},
        {"type": "global_labels", "labels": [("VCC_12V", 263, 30, 180, "output")]},
        # Y1 oscillator
        {"type": "symbol", "model": "osc_diff", "ref": "Y1", "value": "100MHz Diff", "x": 200, "y": 270},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 190, 272.54, 180, "input"),
            ("GND", 190, 267.46, 180, "input"),
        ]},
        {"type": "clock_fan", "x_base": 210, "y_base": 270, "count": 8},
        # Status LEDs
        {"type": "led_block", "names": ["PWR_GREEN", "ACT_BLUE", "ERR_RED", "LINK_YEL"],
         "lx": 10, "ly_base": 10, "ly_step": 12},
        # Decoupling caps C1-C16
        {"type": "bp_decoupling_grid", "count": 16, "cols": 8,
         "x_base": 100, "y_base": 280, "x_step": 12, "y_step": 8, "vcc_label": "VCC_12V"},
        # Bulk caps C17-C24
        {"type": "bulk_caps", "count": 8, "model": "cap_1206", "value": "47uF",
         "x_base": 60, "y": 300, "x_step": 15, "vcc_label": "VCC_12V"},
    ],
}

NODE_PCB_DEF = {
    "width": 170, "height": 100, "layers": 6,
    "title_text": "Hydra Mesh v6.0 -- Compute Node", "title_x": 85, "title_y": 4,
    "mounting_holes": [(3, 3), (167, 3), (3, 97), (167, 97)],
    "footprints": [
        (MODELS["ti180"]["footprint"], "U1", "Ti180", 25, 30),
        (MODELS["lpddr4"]["footprint"], "U2", "LPDDR4", 25, 12),
        (MODELS["w25q128"]["footprint"], "U3", "W25Q128", 40, 30),
        (MODELS["pcie_switch_4port"]["footprint"], "U12", "PI7C9X2G404SL", 55, 50),
        (MODELS["arc_a380"]["footprint"], "U4", "Arc A380 #1", 95, 15),
        (MODELS["arc_a380"]["footprint"], "U5", "Arc A380 #2", 95, 42),
        (MODELS["arc_a380"]["footprint"], "U6", "Arc A380 #3", 140, 15),
        (MODELS["arc_a380"]["footprint"], "U7", "Arc A380 #4", 140, 42),
        (MODELS["ir3842"]["footprint"], "U13", "IR3842 #1", 90, 70),
        (MODELS["ir3842"]["footprint"], "U14", "IR3842 #2", 110, 70),
        (MODELS["ir3842"]["footprint"], "U15", "IR3842 #3", 130, 70),
        (MODELS["ir3842"]["footprint"], "U16", "IR3842 #4", 150, 70),
        (MODELS["tlv62130"]["footprint"], "U17", "TLV62130 0.9V", 25, 70),
        (MODELS["tlv62130"]["footprint"], "U18", "TLV62130 1.8V", 40, 70),
        (MODELS["ap2112k"]["footprint"], "U19", "AP2112K", 55, 70),
        (MODELS["osc_diff"]["footprint"], "Y1", "100MHz", 55, 30),
        (MODELS["serdes_conn"]["footprint"], "J1", "SERDES", 5, 50),
        (MODELS["led"]["footprint"], "D1", "PWR", 5, 10),
        (MODELS["led"]["footprint"], "D2", "ACT", 10, 10),
        (MODELS["resistor"]["footprint"], "R1", "330", 5, 13),
        (MODELS["resistor"]["footprint"], "R2", "330", 10, 13),
        (MODELS["resistor"]["footprint"], "R3", "10k", 15, 45),
        (MODELS["resistor"]["footprint"], "R4", "10k", 20, 45),
        (MODELS["resistor"]["footprint"], "R5", "10k", 25, 45),
        (MODELS["resistor"]["footprint"], "R6", "10k", 30, 45),
        (MODELS["inductor"]["footprint"], "L1", "4.7uH", 90, 78),
        (MODELS["inductor"]["footprint"], "L2", "4.7uH", 110, 78),
        (MODELS["inductor"]["footprint"], "L3", "4.7uH", 130, 78),
        (MODELS["inductor"]["footprint"], "L4", "4.7uH", 150, 78),
        (MODELS["inductor"]["footprint"], "L5", "4.7uH", 15, 78),
    ],
    "cap_steps": [
        {"fn": _node_pcb_ic_caps,
         "ic_positions": [(25, 30), (25, 12), (40, 30), (55, 50),
                          (95, 15), (95, 42), (140, 15), (140, 42),
                          (90, 70), (110, 70), (130, 70), (150, 70)]},
        {"fn": _node_pcb_fill_100nf},
        {"fn": _node_pcb_bulk_caps},
    ],
}

_BP_SLOT_X = [20, 55, 90, 125, 160, 195, 230, 265]

BACKPLANE_PCB_DEF = {
    "width": 300, "height": 120, "layers": 4,
    "title_text": "Hydra Mesh v6.0 -- Rack Backplane", "title_x": 150, "title_y": 4,
    "mounting_holes": [(5, 5), (295, 5), (5, 115), (295, 115)],
    "footprints": (
        [(MODELS["serdes_conn"]["footprint"], f"J{i+1}", f"Slot {i+1}", sx, 60) for i, sx in enumerate(_BP_SLOT_X)]
        + [
            (MODELS["power_conn"]["footprint"], "J10", "12V In", 290, 10),
            (MODELS["polyfuse"]["footprint"], "F1", "15A", 280, 10),
            (MODELS["osc_diff"]["footprint"], "Y1", "100MHz", 150, 10),
        ]
        + [(MODELS["led"]["footprint"], f"D{i+1}", f"LED{i+1}", 10 + i * 5, 10) for i in range(4)]
        + [(MODELS["resistor"]["footprint"], f"R{i+1}", "330", 10 + i * 5, 15) for i in range(4)]
        + [
            (MODELS["pcie_edge_x16"]["footprint"], "J9", "PCIe x16", 0, 60),
        ]
    ),
    "cap_steps": [
        {"fn": _bp_pcb_slot_caps, "slot_x_positions": _BP_SLOT_X},
        {"fn": _bp_pcb_bulk_caps},
    ],
}

# ============================================================
# GENERATION FUNCTIONS (thin wrappers around engine)
# ============================================================

def gen_node_sch(output_dir):
    run_sch_engine(NODE_BOARD, output_dir, "hydra_node.kicad_sch")

def gen_node_pcb(output_dir):
    run_pcb_engine(NODE_PCB_DEF, output_dir, "hydra_node.kicad_pcb")

def gen_backplane_sch(output_dir):
    run_sch_engine(BACKPLANE_BOARD, output_dir, "hydra_backplane.kicad_sch")

def gen_backplane_pcb(output_dir):
    run_pcb_engine(BACKPLANE_PCB_DEF, output_dir, "hydra_backplane.kicad_pcb")

# ============================================================
# MAIN
# ============================================================

def main():
    print("Hydra Mesh v6.0 -- Generating KiCad 8 files...\n")

    node_dir = os.path.join(OUTPUT_DIR, "hydra_node")
    os.makedirs(node_dir, exist_ok=True)
    reset_uuids()
    gen_node_sch(node_dir)
    gen_node_pcb(node_dir)

    bp_dir = os.path.join(OUTPUT_DIR, "hydra_backplane")
    os.makedirs(bp_dir, exist_ok=True)
    reset_uuids()
    gen_backplane_sch(bp_dir)
    gen_backplane_pcb(bp_dir)

    print("\nDone! Generated files:")
    for d in [node_dir, bp_dir]:
        for root, dirs, files in os.walk(d):
            for f in sorted(files):
                fp = os.path.join(root, f)
                print(f"  {fp}  ({os.path.getsize(fp)} bytes)")

if __name__ == "__main__":
    main()
