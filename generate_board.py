#!/usr/bin/env python3
"""Hydra Mesh v7.0 -- 4x ECP5 Inference Node
Generates KiCad 8 files for 4x ECP5 inference node + rack backplane."""

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

def sch_header(paper="A1", title="", rev="7.0"):
    u = nuuid()
    lines = [
        "(kicad_sch",
        "  (version 20231120)",
        '  (generator "hydra_v7_gen")',
        '  (generator_version "7.0")',
        f'  (uuid "{u}")',
        f'  (paper "{paper}")',
        "  (title_block",
        f'    (title "{title}")',
        f'    (rev "{rev}")',
        '    (company "Hydra Mesh v7.0")',
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

def lib_symbol_rp2354b():
    pins = []
    spi0 = [("SPI0_SCK","S1"),("SPI0_MOSI","S2"),("SPI0_MISO","S3"),("SPI0_CS","S4")]
    for i, (name, num) in enumerate(spi0):
        pins.append(_pin("bidirectional", 0, -30.48, 45.72 - i * 2.54, name, num, 1.016))
    spi1 = [("SPI1_SCK","S5"),("SPI1_MOSI","S6"),("SPI1_MISO","S7"),("SPI1_CS","S8")]
    for i, (name, num) in enumerate(spi1):
        pins.append(_pin("bidirectional", 0, -30.48, 33.02 - i * 2.54, name, num, 1.016))
    for i in range(48):
        pins.append(_pin("bidirectional", 180 if i >= 24 else 0,
                         30.48 if i >= 24 else -30.48,
                         20.32 - (i % 24) * 2.54,
                         f"GPIO{i}", f"G{i+1}", 1.016))
    usb = [("USB_DP","U1"),("USB_DN","U2")]
    for i, (name, num) in enumerate(usb):
        pins.append(_pin("bidirectional", 180, 30.48, -40.64 - i * 2.54, name, num, 1.016))
    qspi = [("QSPI_SCK","Q1"),("QSPI_CS","Q2"),("QSPI_D0","Q3"),("QSPI_D1","Q4"),
             ("QSPI_D2","Q5"),("QSPI_D3","Q6")]
    for i, (name, num) in enumerate(qspi):
        pins.append(_pin("bidirectional", 180, 30.48, -46.74 - i * 2.54, name, num, 1.016))
    sdio = [("SDIO_CLK","SD1"),("SDIO_CMD","SD2"),
            ("SDIO_D0","SD3"),("SDIO_D1","SD4"),("SDIO_D2","SD5"),("SDIO_D3","SD6")]
    for i, (name, num) in enumerate(sdio):
        pins.append(_pin("bidirectional", 0, -30.48, -35.56 - i * 2.54, name, num, 1.016))
    swd = [("SWD_CLK","SW1","input"),("SWD_DIO","SW2","bidirectional")]
    for i, (name, num, pt) in enumerate(swd):
        pins.append(_pin(pt, 180, 30.48, -62.74 - i * 2.54, name, num, 1.016))
    xtal = [("XIN","X1","input"),("XOUT","X2","output")]
    for i, (name, num, pt) in enumerate(xtal):
        pins.append(_pin(pt, 0, -30.48, -50.8 - i * 2.54, name, num, 1.016))
    pwr = [("VCC","P1","power_in"),("VCC_IO","P2","power_in"),
           ("GND","P3","power_in"),("GND","P4","power_in"),
           ("GND","P5","power_in"),("GND","P6","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 30.48, -68.58 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "MCU_RaspberryPi:RP2354B" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 50.8 0) (effects (font (size 1.27 1.27))))
      (property "Value" "RP2354B" (at 0 -83.82 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.5mm_EP5.6x5.6mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "RP2354B_0_1"
        (rectangle (start -27.94 48.26) (end 27.94 -81.28) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "RP2354B_1_1"
{pins_str}
      )
    )"""

def lib_symbol_ecp5():
    pins = []
    # DDR3 bus
    for i in range(16):
        pins.append(_pin("bidirectional", 0, -30.48, 60.96 - i * 2.54, f"DDR3_DQ{i}", f"D{i+1}", 1.016))
    for i in range(15):
        pins.append(_pin("output", 0, -30.48, 19.04 - i * 2.54, f"DDR3_A{i}", f"A{i+1}", 1.016))
    for i in range(3):
        pins.append(_pin("output", 0, -30.48, -20.32 - i * 2.54, f"DDR3_BA{i}", f"BA{i+1}", 1.016))
    ddr_ctrl = [("DDR3_CK","DC1","output"),("DDR3_CKE","DC2","output"),
                ("DDR3_CS","DC3","output"),("DDR3_RAS","DC4","output"),
                ("DDR3_CAS","DC5","output"),("DDR3_WE","DC6","output"),
                ("DDR3_ODT","DC7","output"),("DDR3_RESET","DC8","output"),
                ("DDR3_DM","DC9","output"),("DDR3_DQS","DC10","bidirectional")]
    for i, (name, num, pt) in enumerate(ddr_ctrl):
        pins.append(_pin(pt, 0, -30.48, -27.94 - i * 2.54, name, num, 1.016))
    # SPI slave
    spi_s = [("SPI_SCK","SP1","input"),("SPI_MOSI","SP2","input"),
             ("SPI_MISO","SP3","output"),("SPI_CS","SP4","input")]
    for i, (name, num, pt) in enumerate(spi_s):
        pins.append(_pin(pt, 180, 30.48, 60.96 - i * 2.54, name, num, 1.016))
    # SERDES TX/RX 4 lanes
    for lane in range(4):
        y = 48.26 - lane * 10.16
        pins.append(_pin("output", 180, 30.48, y, f"SERDES_TXP{lane}", f"ST{lane*2+1}", 1.016))
        pins.append(_pin("output", 180, 30.48, y - 2.54, f"SERDES_TXN{lane}", f"ST{lane*2+2}", 1.016))
        pins.append(_pin("input", 180, 30.48, y - 5.08, f"SERDES_RXP{lane}", f"SR{lane*2+1}", 1.016))
        pins.append(_pin("input", 180, 30.48, y - 7.62, f"SERDES_RXN{lane}", f"SR{lane*2+2}", 1.016))
    # JTAG
    jtag = [("TCK","E1","input"),("TMS","E2","input"),("TDI","E3","input"),("TDO","E4","output"),
            ("PROGRAMN","E5","input"),("INITN","E6","output"),("DONE","E7","output")]
    for i, (name, num, pt) in enumerate(jtag):
        pins.append(_pin(pt, 180, 30.48, -5.08 - i * 2.54, name, num, 1.016))
    # Config SPI master (for boot flash)
    cfg = [("CFG_SCK","CF1","output"),("CFG_MOSI","CF2","output"),
           ("CFG_MISO","CF3","input"),("CFG_CS","CF4","output")]
    for i, (name, num, pt) in enumerate(cfg):
        pins.append(_pin(pt, 180, 30.48, -25.4 - i * 2.54, name, num, 1.016))
    # Refclk input
    pins.append(_pin("input", 0, -30.48, -53.34, "REFCLK_P", "CK1", 1.016))
    pins.append(_pin("input", 0, -30.48, -55.88, "REFCLK_N", "CK2", 1.016))
    # Power
    pwr = [("VCC","P1","power_in"),("VCCIO","P2","power_in"),
           ("VCCAUX","P3","power_in"),("GND","P4","power_in"),
           ("GND","P5","power_in"),("GND","P6","power_in"),
           ("GND","P7","power_in"),("GND","P8","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 0, -30.48, -60.96 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "FPGA_Lattice:ECP5UM5G-85F" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 66.04 0) (effects (font (size 1.27 1.27))))
      (property "Value" "ECP5UM5G-85F" (at 0 -81.28 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-554_1.0mm_24x24_25.0x25.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "ECP5UM5G-85F_0_1"
        (rectangle (start -27.94 63.5) (end 27.94 -78.74) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "ECP5UM5G-85F_1_1"
{pins_str}
      )
    )"""

def lib_symbol_ddr3():
    pins = []
    for i in range(16):
        pins.append(_pin("bidirectional", 0, -17.78, 33.02 - i * 2.54, f"DQ{i}", f"D{i+1}", 1.016))
    for i in range(15):
        pins.append(_pin("input", 180, 17.78, 33.02 - i * 2.54, f"A{i}", f"A{i+1}", 1.016))
    for i in range(3):
        pins.append(_pin("input", 180, 17.78, -5.08 - i * 2.54, f"BA{i}", f"BA{i+1}", 1.016))
    ctrl = [("CK","CK1","input"),("CKE","CK2","input"),("CS","CS1","input"),
            ("RAS","CT1","input"),("CAS","CT2","input"),("WE","CT3","input"),
            ("ODT","CT4","input"),("RESET","CT5","input"),
            ("DM","CT6","input"),("DQS","DQS1","bidirectional")]
    for i, (name, num, pt) in enumerate(ctrl):
        pins.append(_pin(pt, 180, 17.78, -12.7 - i * 2.54, name, num, 1.016))
    pwr = [("VDD","V1","power_in"),("VDDQ","V2","power_in"),
           ("VREF","V3","power_in"),("VSS","V4","power_in"),
           ("VSS","V5","power_in"),("VSS","V6","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 0, -17.78, -10.16 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Memory_DDR3:DDR3_512MB_x16" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 38.1 0) (effects (font (size 1.27 1.27))))
      (property "Value" "DDR3_512MB_x16" (at 0 -38.1 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-96_0.8mm_8x12_7.5x10.5mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "DDR3_512MB_x16_0_1"
        (rectangle (start -15.24 35.56) (end 15.24 -35.56) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "DDR3_512MB_x16_1_1"
{pins_str}
      )
    )"""

def lib_symbol_w6100():
    pins = []
    spi = [("SPI_SCK","S1","input"),("SPI_MOSI","S2","input"),
           ("SPI_MISO","S3","output"),("SPI_CS","S4","input")]
    for i, (name, num, pt) in enumerate(spi):
        pins.append(_pin(pt, 0, -15.24, 10.16 - i * 2.54, name, num, 1.016))
    eth = [("TXP","E1","output"),("TXN","E2","output"),
           ("RXP","E3","input"),("RXN","E4","input")]
    for i, (name, num, pt) in enumerate(eth):
        pins.append(_pin(pt, 180, 15.24, 10.16 - i * 2.54, name, num, 1.016))
    pins.append(_pin("output", 0, -15.24, -2.54, "INT", "INT1", 1.016))
    pins.append(_pin("input", 0, -15.24, -5.08, "RST", "RST1", 1.016))
    pwr = [("VCC","P1","power_in"),("GND","P2","power_in"),("GND","P3","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 15.24, -2.54 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Interface_Ethernet:W6100" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 15.24 0) (effects (font (size 1.27 1.27))))
      (property "Value" "W6100" (at 0 -12.7 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.6x5.6mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "W6100_0_1"
        (rectangle (start -12.7 12.7) (end 12.7 -10.16) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "W6100_1_1"
{pins_str}
      )
    )"""

def lib_symbol_emmc():
    pins = []
    sig = [("CLK","E1","input"),("CMD","E2","bidirectional"),
           ("DAT0","E3","bidirectional"),("DAT1","E4","bidirectional"),
           ("DAT2","E5","bidirectional"),("DAT3","E6","bidirectional"),
           ("RST","E7","input")]
    for i, (name, num, pt) in enumerate(sig):
        pins.append(_pin(pt, 0, -12.7, 7.62 - i * 2.54, name, num, 1.016))
    pwr = [("VCC","V1","power_in"),("VCCQ","V2","power_in"),("GND","V3","power_in"),("GND","V4","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 12.7, 5.08 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Memory_Flash:eMMC_8GB" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "eMMC_8GB" (at 0 -12.7 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-153_11.5x13.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "eMMC_8GB_0_1"
        (rectangle (start -10.16 10.16) (end 10.16 -10.16) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "eMMC_8GB_1_1"
{pins_str}
      )
    )"""

def lib_symbol_crystal_25mhz():
    pins = "\n".join([
        _pin("passive", 0, -7.62, 0, "XIN", "1"),
        _pin("passive", 180, 7.62, 0, "XOUT", "3"),
        _pin("power_in", 0, -7.62, -2.54, "GND", "2"),
        _pin("power_in", 180, 7.62, -2.54, "GND", "4"),
    ])
    return f"""    (symbol "Device:Crystal_GND24" (in_bom yes) (on_board yes)
      (property "Reference" "Y" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Value" "25MHz" (at 0 -5.08 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Crystal:Crystal_SMD_3215-4Pin_3.2x1.5mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Crystal_GND24_0_1"
        (rectangle (start -2.54 2.54) (end 2.54 -2.54) (stroke (width 0.254) (type default)) (fill (type none)))
      )
      (symbol "Crystal_GND24_1_1"
{pins}
      )
    )"""

def lib_symbol_w25q32():
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
    return f"""    (symbol "Memory_Flash:W25Q32JV" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "W25Q32JV" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:SOP-8_3.9x4.9mm_P1.27mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "W25Q32JV_0_1"
        (rectangle (start -7.62 5.08) (end 7.62 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "W25Q32JV_1_1"
{pins}
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

def lib_symbol_tps54560():
    pins = "\n".join([
        _pin("power_in", 0, -12.7, 7.62, "VIN", "1"),
        _pin("input", 0, -12.7, 5.08, "EN", "2"),
        _pin("output", 0, -12.7, 2.54, "BOOT", "3"),
        _pin("output", 0, -12.7, 0, "PH", "4"),
        _pin("input", 180, 12.7, 7.62, "FB", "5"),
        _pin("passive", 180, 12.7, 5.08, "COMP", "6"),
        _pin("input", 180, 12.7, 2.54, "SS/TR", "7"),
        _pin("output", 180, 12.7, 0, "PGOOD", "8"),
        _pin("power_in", 180, 12.7, -2.54, "GND", "9"),
        _pin("power_in", 180, 12.7, -5.08, "EPAD", "10"),
    ])
    return f"""    (symbol "Regulator_Switching:TPS54560" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "TPS54560" (at 0 -10.16 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_SO:HSOP-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x2.41mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "TPS54560_0_1"
        (rectangle (start -10.16 10.16) (end 10.16 -7.62) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "TPS54560_1_1"
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
  (generator "hydra_v7_gen")
  (generator_version "7.0")
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
    "rp2354b": {
        "lib_id": "MCU_RaspberryPi:RP2354B",
        "lib_symbol_fn": lib_symbol_rp2354b,
        "footprint": "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.5mm_EP5.6x5.6mm",
    },
    "ecp5": {
        "lib_id": "FPGA_Lattice:ECP5UM5G-85F",
        "lib_symbol_fn": lib_symbol_ecp5,
        "footprint": "Package_BGA:BGA-554_1.0mm_24x24_25.0x25.0mm",
    },
    "ddr3": {
        "lib_id": "Memory_DDR3:DDR3_512MB_x16",
        "lib_symbol_fn": lib_symbol_ddr3,
        "footprint": "Package_BGA:BGA-96_0.8mm_8x12_7.5x10.5mm",
    },
    "w6100": {
        "lib_id": "Interface_Ethernet:W6100",
        "lib_symbol_fn": lib_symbol_w6100,
        "footprint": "Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.6x5.6mm",
    },
    "emmc": {
        "lib_id": "Memory_Flash:eMMC_8GB",
        "lib_symbol_fn": lib_symbol_emmc,
        "footprint": "Package_BGA:BGA-153_11.5x13.0mm",
    },
    "crystal_25mhz": {
        "lib_id": "Device:Crystal_GND24",
        "lib_symbol_fn": lib_symbol_crystal_25mhz,
        "footprint": "Crystal:Crystal_SMD_3215-4Pin_3.2x1.5mm",
    },
    "w25q128": {
        "lib_id": "Memory_Flash:W25Q128JV",
        "lib_symbol_fn": lib_symbol_w25q128,
        "footprint": "Package_SO:SOP-8_3.9x4.9mm_P1.27mm",
    },
    "w25q32": {
        "lib_id": "Memory_Flash:W25Q32JV",
        "lib_symbol_fn": lib_symbol_w25q32,
        "footprint": "Package_SO:SOP-8_3.9x4.9mm_P1.27mm",
    },
    "tps54560": {
        "lib_id": "Regulator_Switching:TPS54560",
        "lib_symbol_fn": lib_symbol_tps54560,
        "footprint": "Package_SO:HSOP-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x2.41mm",
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

def _handle_fpga_array(ctx, sp):
    """Place 4x ECP5 FPGAs with per-FPGA DDR3, W25Q32, SPI slave, and decoupling."""
    positions = sp["positions"]
    fpga_refs = sp["fpga_refs"]
    ddr_refs = sp["ddr_refs"]
    flash_refs = sp["flash_refs"]
    for i, ((fx, fy), fref, dref, flref) in enumerate(
            zip(positions, fpga_refs, ddr_refs, flash_refs)):
        # ECP5 FPGA
        ctx["s"] += place_symbol("FPGA_Lattice:ECP5UM5G-85F", fref,
                                  f"ECP5UM5G-85F #{i}", fx, fy)
        # DDR3 per FPGA
        ctx["s"] += place_symbol("Memory_DDR3:DDR3_512MB_x16", dref,
                                  f"DDR3 512MB #{i}", fx - 70, fy + 10)
        # W25Q32 bitstream flash per FPGA
        ctx["s"] += place_symbol("Memory_Flash:W25Q32JV", flref,
                                  f"W25Q32 FPGA{i}", fx + 45, fy - 20)
        # DDR3 bus labels (ECP5 ↔ DDR3)
        ddr_dq = [f"DQ{d}" for d in range(16)]
        ddr_addr = [f"A{a}" for a in range(15)]
        ddr_ba = [f"BA{b}" for b in range(3)]
        ddr_ctrl = ["CK","CKE","CS","RAS","CAS","WE","ODT","RESET","DM","DQS"]
        all_ddr = ddr_dq + ddr_addr + ddr_ba + ddr_ctrl
        for j, sig in enumerate(all_ddr):
            label_name = f"FPGA{i}_DDR3_{sig}"
            ctx["s"] += place_label(label_name, fx - 33, fy + 60.96 - j * 2.54)
            if j < 16:
                ctx["s"] += place_label(label_name, fx - 70 - 20, fy + 10 + 33.02 - j * 2.54)
            elif j < 31:
                ctx["s"] += place_label(label_name, fx - 70 + 20, fy + 10 + 33.02 - (j - 16) * 2.54)
            elif j < 34:
                ctx["s"] += place_label(label_name, fx - 70 + 20, fy + 10 - 5.08 - (j - 31) * 2.54)
            else:
                ctx["s"] += place_label(label_name, fx - 70 + 20, fy + 10 - 12.7 - (j - 34) * 2.54)
        # Config SPI labels (ECP5 master → W25Q32)
        cfg_sigs = ["SCK","MOSI","MISO","CS"]
        for j, sig in enumerate(cfg_sigs):
            label_name = f"FPGA{i}_CFG_{sig}"
            ctx["s"] += place_label(label_name, fx + 33, fy - 25.4 - j * 2.54)
            ctx["s"] += place_label(label_name, fx + 45 + (10 if j < 2 else -13),
                                     fy - 20 + (3.81 - j * 2.54 if j >= 2 else 1.27 + (1 - j) * 2.54))
        # SPI slave labels (RP2354B → ECP5)
        spi_sigs = ["SCK","MOSI","MISO","CS"]
        for j, sig in enumerate(spi_sigs):
            ctx["s"] += place_global_label(f"FPGA{i}_SPI_{sig}",
                                            fx + 33, fy + 60.96 - j * 2.54, 0, "bidirectional")
        # SERDES labels
        for lane in range(4):
            for suffix in ["TXP","TXN","RXP","RXN"]:
                ctx["s"] += place_global_label(
                    f"FPGA{i}_SERDES_{suffix}{lane}",
                    fx + 33, fy + 48.26 - lane * 10.16 - (["TXP","TXN","RXP","RXN"].index(suffix)) * 2.54,
                    0, "bidirectional")
        # Refclk
        ctx["s"] += place_global_label(f"ECP5_REFCLK_P", fx - 33, fy - 53.34, 180, "input")
        ctx["s"] += place_global_label(f"ECP5_REFCLK_N", fx - 33, fy - 55.88, 180, "input")
        # Power
        ctx["s"] += place_global_label("VCC_1V1", fx - 33, fy - 60.96, 180, "input")
        ctx["s"] += place_global_label("VCC_3V3", fx - 33, fy - 63.5, 180, "input")
        ctx["s"] += place_global_label("VCC_2V5", fx - 33, fy - 66.04, 180, "input")
        ctx["s"] += place_global_label("GND", fx - 33, fy - 68.58, 180, "input")
        ctx["s"] += place_global_label("GND", fx - 33, fy - 71.12, 180, "input")
        # DDR3 power
        ctx["s"] += place_global_label("VCC_1V5", fx - 70 - 20, fy + 10 - 10.16, 180, "input")
        ctx["s"] += place_global_label("VCC_1V5", fx - 70 - 20, fy + 10 - 12.7, 180, "input")
        ctx["s"] += place_global_label("VCC_1V8", fx - 70 - 20, fy + 10 - 15.24, 180, "input")
        ctx["s"] += place_global_label("GND", fx - 70 - 20, fy + 10 - 17.78, 180, "input")
        # Flash power
        ctx["s"] += place_global_label("VCC_3V3", fx + 45 + 13, fy - 20 - 3.81, 0, "input")
        ctx["s"] += place_global_label("GND", fx + 45 - 13, fy - 20 - 3.81, 180, "input")
        # Per-FPGA decoupling (4 caps)
        for dc in range(4):
            ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF",
                                      fx + 40 + dc * 7, fy + 30)
            ctx["s"] += place_global_label("VCC_1V1", fx + 40 + dc * 7, fy + 32.54, 0, "input")
            ctx["s"] += place_global_label("GND", fx + 40 + dc * 7, fy + 27.46, 0, "input")
            ctx["c_idx"] += 1

def _handle_serdes_ring(ctx, sp):
    """Wire inter-FPGA SERDES ring: 0→1→2→3→0, 4 lanes per link."""
    ring = sp["ring"]
    for src, dst in ring:
        for lane in range(4):
            for suffix_pair in [("TXP","RXP"),("TXN","RXN")]:
                tx_label = f"RING_{src}_{dst}_L{lane}_{suffix_pair[0]}"
                ctx["s"] += place_global_label(
                    tx_label, sp["x_base"] + src * 20, sp["y_base"] + lane * 5.08 + (0 if suffix_pair[0]=="TXP" else 2.54),
                    0, "bidirectional")
                ctx["s"] += place_global_label(
                    tx_label, sp["x_base"] + dst * 20, sp["y_base"] + lane * 5.08 + (0 if suffix_pair[0]=="TXP" else 2.54) + 25,
                    0, "bidirectional")

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
    "global_labels": _handle_global_labels,
    "global_label_fan": _handle_global_label_fan,
    "wire": _handle_wire,
    "decoupling": _handle_decoupling,
    "pull_ups": _handle_pull_ups,
    "fpga_array": _handle_fpga_array,
    "serdes_ring": _handle_serdes_ring,
    "regulator_block": _handle_regulator_block,
    "bulk_caps": _handle_bulk_caps,
    "fill_caps": _handle_fill_caps,
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
    limit = step.get("limit", 80)
    while ctx["c_idx"] <= limit:
        col = (ctx["c_idx"] - 1) % 15
        row = (ctx["c_idx"] - 1) // 15
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                   f"C{ctx['c_idx']}", "100nF", 10 + col * 12, 120 + row * 3)
        ctx["c_idx"] += 1

def _node_pcb_bulk_caps(step, ctx):
    limit = step.get("limit", 120)
    while ctx["c_idx"] <= limit:
        col = (ctx["c_idx"] - 81) % 10
        row = (ctx["c_idx"] - 81) // 10
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0805_2012Metric",
                                   f"C{ctx['c_idx']}", "10uF", 15 + col * 18, 130 + row * 4)
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

_SERDES_8_SIGNALS = [
    ("SERDES_TXP0", 48.26), ("SERDES_TXN0", 45.72),
    ("SERDES_RXP0", 43.18), ("SERDES_RXN0", 40.64),
    ("SERDES_TXP1", 35.56), ("SERDES_TXN1", 33.02),
    ("SERDES_RXP1", 30.48), ("SERDES_RXN1", 27.94),
]

NODE_BOARD = {
    "project_name": "hydra_node",
    "header": {"paper": "A0", "title": "Hydra Mesh v7.0 -- 4x ECP5 Inference Node", "rev": "7.0"},
    "lib_symbol_order": [
        "rp2354b", "ecp5", "ddr3", "w6100", "emmc", "crystal_25mhz",
        "w25q128", "w25q32", "tps54560", "ap2112k", "osc_diff", "serdes_conn",
        "resistor", "cap", "cap_0805", "led", "inductor",
    ],
    "splices": [
        {"type": "text", "text": "HYDRA NODE v7.0 -- RP2354B + 4x ECP5 Inference Array", "x": 250, "y": 10, "size": 3.0},

        # === Section 1: RP2354B + crystal + flash + SWD header ===
        {"type": "text", "text": "RP2354B Host Controller + 25MHz Crystal + 16MB Flash + SWD", "x": 80, "y": 30, "size": 2.0},
        {"type": "symbol", "model": "rp2354b", "ref": "U1", "value": "RP2354B QFN-80 @400MHz", "x": 80, "y": 80},
        {"type": "symbol", "model": "crystal_25mhz", "ref": "Y1", "value": "25MHz Crystal", "x": 35, "y": 35},
        # Crystal → RP2354B XIN/XOUT
        {"type": "bus_labels", "prefix": "MCU_", "signals": ["XIN","XOUT"],
         "a_x": 45, "a_y_base": 35, "a_y_step": -2.54,
         "b_x": 47, "b_y_base": 29.2, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("GND", 25, 32.46, 180, "input"), ("GND", 45, 32.46, 0, "input"),
        ]},
        # W25Q128 flash for RP2354B firmware
        {"type": "symbol", "model": "w25q128", "ref": "U2", "value": "W25Q128JV 16MB", "x": 130, "y": 35},
        # QSPI labels
        {"type": "bus_labels", "prefix": "QSPI_", "signals": ["SCK","CS","D0","D1","D2","D3"],
         "a_x": 113, "a_y_base": 33.26, "a_y_step": -2.54,
         "b_x": 140, "b_y_base": 38.81, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 143, 31.19, 0, "input"),
            ("GND", 117, 31.19, 180, "input"),
        ]},
        # USB-C
        {"type": "global_label_fan", "signals": ["USB_DP","USB_DN"],
         "x": 113, "y_base": 39.36, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # SWD header
        {"type": "global_label_fan", "signals": ["SWD_CLK","SWD_DIO"],
         "x": 113, "y_base": 17.26, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # RP2354B power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 113, 11.42, 0, "input"),
            ("VCC_3V3", 113, 8.88, 0, "input"),
            ("GND", 113, 6.34, 0, "input"),
            ("GND", 113, 3.8, 0, "input"),
        ]},
        # RP2354B decoupling
        {"type": "decoupling", "x_base": 120, "y": 50, "count": 2, "x_step": 7,
         "vcc_label": "VCC_3V3", "bulk": True, "bulk_x": 120, "bulk_y": 60},

        # === Section 2: 4x ECP5 FPGA array (2x2 layout) ===
        {"type": "text", "text": "ECP5 FPGA Inference Array (4x ECP5UM5G-85F)", "x": 250, "y": 55, "size": 2.0},
        {"type": "fpga_array",
         "positions": [(250, 100), (450, 100), (250, 250), (450, 250)],
         "fpga_refs": ["U3", "U4", "U5", "U6"],
         "ddr_refs": ["U7", "U8", "U9", "U10"],
         "flash_refs": ["U11", "U12", "U13", "U14"]},

        # === Section 5: Inter-FPGA SERDES mesh (ring: 0→1→2→3→0) ===
        {"type": "text", "text": "Inter-FPGA SERDES Ring (4 lanes/link @ 5Gbps)", "x": 250, "y": 360, "size": 2.0},
        {"type": "serdes_ring",
         "ring": [(0, 1), (1, 2), (2, 3), (3, 0)],
         "x_base": 250, "y_base": 375},

        # === Section 6: W6100 Ethernet + RJ45 ===
        {"type": "text", "text": "W6100 Gigabit Ethernet + RJ45 MagJack", "x": 80, "y": 155, "size": 2.0},
        {"type": "symbol", "model": "w6100", "ref": "U15", "value": "W6100 GbE", "x": 80, "y": 180},
        # SPI labels (RP2354B → W6100)
        {"type": "global_label_fan", "signals": ["W6100_SPI_SCK","W6100_SPI_MOSI","W6100_SPI_MISO","W6100_SPI_CS"],
         "x": 62, "y_base": 190.16, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # Ethernet signals
        {"type": "global_label_fan", "signals": ["ETH_TXP","ETH_TXN","ETH_RXP","ETH_RXN"],
         "x": 98, "y_base": 190.16, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # W6100 INT/RST
        {"type": "global_label_fan", "signals": ["W6100_INT","W6100_RST"],
         "x": 62, "y_base": 177.62, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # W6100 power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 98, 177.46, 0, "input"),
            ("GND", 98, 174.92, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 105, "y": 180, "count": 2, "x_step": 7,
         "vcc_label": "VCC_3V3"},

        # === Section 7: eMMC storage ===
        {"type": "text", "text": "8GB eMMC Storage (SDIO 4-bit)", "x": 80, "y": 205, "size": 2.0},
        {"type": "symbol", "model": "emmc", "ref": "U16", "value": "eMMC 8GB", "x": 80, "y": 225},
        # SDIO labels (RP2354B → eMMC)
        {"type": "bus_labels", "prefix": "SDIO_", "signals": ["CLK","CMD","D0","D1","D2","D3"],
         "a_x": 47, "a_y_base": 44.44, "a_y_step": -2.54,
         "b_x": 65, "b_y_base": 232.62, "b_y_step": -2.54},
        # eMMC RST
        {"type": "global_labels", "labels": [("EMMC_RST", 65, 217.38, 180, "bidirectional")]},
        # eMMC power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 95, 230.08, 0, "input"),
            ("VCC_1V8", 95, 227.54, 0, "input"),
            ("GND", 95, 225.0, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 105, "y": 225, "count": 2, "x_step": 7,
         "vcc_label": "VCC_3V3"},

        # === Section 8: Power supply ===
        {"type": "text", "text": "POWER SUPPLY -- 12V DC Input", "x": 80, "y": 270, "size": 2.0},
        # J2 power input (barrel jack / screw terminal)
        {"type": "symbol", "model": "power_conn", "ref": "J2", "value": "12V DC Input", "x": 60, "y": 290},
        {"type": "global_labels", "labels": [
            ("VCC_12V", 70, 292.54, 0, "output"),
            ("GND", 70, 287.46, 0, "input"),
        ]},
        # F1 polyfuse
        {"type": "symbol", "model": "polyfuse", "ref": "F1", "value": "5A Polyfuse", "x": 90, "y": 292},
        {"type": "wire", "x1": 70, "y1": 292.54, "x2": 85, "y2": 292},
        {"type": "global_labels", "labels": [("VCC_12V_FUSED", 98, 292, 0, "output")]},
        # TPS54560 #1: 12V→3.3V/5A
        {"type": "regulator_block", "model": "tps54560", "ref": "U17", "value": "TPS54560 12V→3.3V/5A",
         "x": 140, "y": 290,
         "globals": [
             ("VCC_12V_FUSED", 125, 297.62, 180, "input"),
             ("VCC_3V3", 155, 297.62, 0, "output"),
             ("GND", 155, 284.92, 0, "input"),
         ],
         "caps": [
             ("cap", 162, 290, ("VCC_3V3", 162, 292.54, 0, "input"), ("GND", 162, 287.46, 0, "input")),
             ("cap_0805", 169, 290, ("VCC_3V3", 169, 292.54, 0, "input"), ("GND", 169, 287.46, 0, "input")),
         ]},
        # L1 for TPS54560 #1
        {"type": "symbol", "model": "inductor", "ref": "L1", "value": "4.7uH", "x": 155, "y": 300},
        # TPS54560 #2: 12V→1.1V/5A (ECP5 core)
        {"type": "regulator_block", "model": "tps54560", "ref": "U18", "value": "TPS54560 12V→1.1V/5A",
         "x": 200, "y": 290,
         "globals": [
             ("VCC_12V_FUSED", 185, 297.62, 180, "input"),
             ("VCC_1V1", 215, 297.62, 0, "output"),
             ("GND", 215, 284.92, 0, "input"),
         ],
         "caps": [
             ("cap", 222, 290, ("VCC_1V1", 222, 292.54, 0, "input"), ("GND", 222, 287.46, 0, "input")),
             ("cap_0805", 229, 290, ("VCC_1V1", 229, 292.54, 0, "input"), ("GND", 229, 287.46, 0, "input")),
         ]},
        # L2 for TPS54560 #2
        {"type": "symbol", "model": "inductor", "ref": "L2", "value": "4.7uH", "x": 215, "y": 300},
        # AP2112K #1: 3.3V→1.8V (DDR3 VTT / misc)
        {"type": "regulator_block", "model": "ap2112k", "ref": "U19", "value": "AP2112K 3.3V→1.8V",
         "x": 260, "y": 290,
         "globals": [
             ("VCC_3V3", 247, 292.54, 180, "input"),
             ("GND", 247, 287.46, 180, "input"),
             ("VCC_1V8", 273, 292.54, 0, "output"),
         ],
         "caps": [
             ("cap", 280, 290, ("VCC_1V8", 280, 292.54, 0, "input"), ("GND", 280, 287.46, 0, "input")),
         ]},
        # AP2112K #2: 3.3V→2.5V (ECP5 auxiliary)
        {"type": "regulator_block", "model": "ap2112k", "ref": "U20", "value": "AP2112K 3.3V→2.5V",
         "x": 310, "y": 290,
         "globals": [
             ("VCC_3V3", 297, 292.54, 180, "input"),
             ("GND", 297, 287.46, 180, "input"),
             ("VCC_2V5", 323, 292.54, 0, "output"),
         ],
         "caps": [
             ("cap", 330, 290, ("VCC_2V5", 330, 292.54, 0, "input"), ("GND", 330, 287.46, 0, "input")),
         ]},
        # DDR3 1.5V derived from 1.8V line (VTT)
        {"type": "global_labels", "labels": [("VCC_1V5", 280, 285, 0, "output")]},
        # Bulk caps near regulators
        {"type": "bulk_caps", "count": 4, "model": "cap_0805", "value": "10uF",
         "x_base": 120, "y": 310, "x_step": 15, "vcc_label": "VCC_12V"},

        # === Section 9: SPI buses (RP2354B → 4x ECP5, RP2354B → W6100) ===
        {"type": "text", "text": "SPI Bus Routing (RP2354B → FPGAs + W6100)", "x": 80, "y": 325, "size": 1.5},
        # RP2354B SPI0 → FPGA0
        {"type": "global_label_fan", "signals": [
            "FPGA0_SPI_SCK","FPGA0_SPI_MOSI","FPGA0_SPI_MISO","FPGA0_SPI_CS"],
         "x": 47, "y_base": 125.72, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # RP2354B SPI1 shared bus → FPGA1,2,3 with GPIO CS
        {"type": "global_label_fan", "signals": [
            "FPGA1_SPI_SCK","FPGA1_SPI_MOSI","FPGA1_SPI_MISO","FPGA1_SPI_CS"],
         "x": 47, "y_base": 113.02, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # FPGA2/3 CS from GPIO
        {"type": "global_labels", "labels": [
            ("FPGA2_SPI_CS", 113, 60.64, 0, "output"),
            ("FPGA3_SPI_CS", 113, 58.1, 0, "output"),
        ]},
        # W6100 SPI from RP2354B GPIO
        {"type": "global_label_fan", "signals": [
            "W6100_SPI_SCK","W6100_SPI_MOSI","W6100_SPI_MISO","W6100_SPI_CS"],
         "x": 113, "y_base": 55.56, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},

        # === Section 10: Status LEDs + misc ===
        {"type": "text", "text": "Status LEDs + Misc", "x": 15, "y": 335, "size": 1.5},
        {"type": "node_led_pair", "led_ref": "D1", "led_value": "PWR_GREEN", "lx": 20, "ly": 350,
         "r_ref": "R1", "r_value": "330", "rx": 10, "ry": 350},
        {"type": "node_led_pair", "led_ref": "D2", "led_value": "SYS_BLUE", "lx": 20, "ly": 362,
         "r_ref": "R2", "r_value": "330", "rx": 10, "ry": 362},
        {"type": "node_led_pair", "led_ref": "D3", "led_value": "ERR_RED", "lx": 20, "ly": 374,
         "r_ref": "R3", "r_value": "330", "rx": 10, "ry": 374},
        {"type": "node_led_pair", "led_ref": "D4", "led_value": "NET_YELLOW", "lx": 20, "ly": 386,
         "r_ref": "R4", "r_value": "330", "rx": 10, "ry": 386},
        # Y2 100MHz diff oscillator for ECP5 reference clock
        {"type": "symbol", "model": "osc_diff", "ref": "Y2", "value": "100MHz Diff", "x": 160, "y": 350},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 150, 352.54, 180, "input"),
            ("GND", 150, 347.46, 180, "input"),
            ("ECP5_REFCLK_P", 170, 352.54, 0, "output"),
            ("ECP5_REFCLK_N", 170, 347.46, 0, "output"),
        ]},
        # SERDES backplane connector
        {"type": "symbol", "model": "serdes_conn", "ref": "J1", "value": "SEAF-40 Backplane", "x": 40, "y": 430},
        {"type": "serdes_conn_labels", "x": 55, "y_base": 430,
         "signals": _SERDES_8_SIGNALS,
         "extra_globals": [
             ("VCC_12V", 25, 478.26, 180, "input"),
             ("GND", 25, 427.46, 180, "input"),
         ]},
        # JTAG pull-ups for ECP5
        {"type": "pull_ups", "signals": ["TCK","TMS","TDI","PROGRAMN"],
         "x_base": 120, "y": 350, "x_step": 7, "vcc_label": "VCC_3V3", "label_prefix": "ECP5_JTAG_", "value": "10k"},
        # Fill 100nF caps to C80
        {"type": "fill_caps", "model": "cap", "value": "100nF", "limit": 80, "cols": 14,
         "x_base": 60, "y_base": 450, "x_step": 8, "y_step": 6,
         "start_offset": lambda idx: ((idx - 1) % 14, (idx - 1) // 14)},
        # Fill 10uF bulk caps C81-C120
        {"type": "fill_caps", "model": "cap_0805", "value": "10uF", "limit": 120, "cols": 12,
         "x_base": 200, "y_base": 450, "x_step": 10, "y_step": 6,
         "start_offset": lambda idx: ((idx - 81) % 12, (idx - 81) // 12)},
    ],
}

_BP_J14_POS = [(150, 40), (150, 100), (150, 160), (150, 220)]
_BP_J58_POS = [(250, 40), (250, 100), (250, 160), (250, 220)]

BACKPLANE_BOARD = {
    "project_name": "hydra_backplane",
    "header": {"paper": "A1", "title": "Hydra Mesh v7.0 -- Rack Backplane (8-slot SERDES)", "rev": "7.0"},
    "lib_symbol_order": [
        "serdes_conn", "power_conn", "polyfuse", "osc_diff",
        "resistor", "cap", "cap_1206", "led",
    ],
    "splices": [
        {"type": "text", "text": "RACK BACKPLANE v7.0 -- 8 Node Slots", "x": 200, "y": 10, "size": 3.0},
        # J1-J4
        {"type": "bp_slot_column", "positions": _BP_J14_POS, "start_idx": 1},
        # J5-J8
        {"type": "bp_slot_column", "positions": _BP_J58_POS, "start_idx": 5},
        # Mesh wiring
        {"type": "mesh_wiring",
         "mesh_pairs": [(1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 8)],
         "all_positions": _BP_J14_POS + _BP_J58_POS},
        # J9 power + F1 polyfuse
        {"type": "symbol", "model": "power_conn", "ref": "J9", "value": "12V Power In", "x": 300, "y": 30},
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

# FPGA positions on PCB: 2x2 grid
_FPGA_PCB_POS = [(50, 30), (120, 30), (50, 80), (120, 80)]
_DDR_PCB_POS = [(30, 15), (100, 15), (30, 65), (100, 65)]

NODE_PCB_DEF = {
    "width": 200, "height": 150, "layers": 6,
    "title_text": "Hydra Mesh v7.0 -- 4x ECP5 Inference Node", "title_x": 100, "title_y": 4,
    "mounting_holes": [(4, 4), (196, 4), (4, 146), (196, 146)],
    "footprints": [
        # RP2354B
        (MODELS["rp2354b"]["footprint"], "U1", "RP2354B", 170, 30),
        # W25Q128 (RP2354B flash)
        (MODELS["w25q128"]["footprint"], "U2", "W25Q128", 185, 20),
        # 4x ECP5
        (MODELS["ecp5"]["footprint"], "U3", "ECP5 #0", 50, 30),
        (MODELS["ecp5"]["footprint"], "U4", "ECP5 #1", 120, 30),
        (MODELS["ecp5"]["footprint"], "U5", "ECP5 #2", 50, 80),
        (MODELS["ecp5"]["footprint"], "U6", "ECP5 #3", 120, 80),
        # 4x DDR3
        (MODELS["ddr3"]["footprint"], "U7", "DDR3 #0", 30, 15),
        (MODELS["ddr3"]["footprint"], "U8", "DDR3 #1", 100, 15),
        (MODELS["ddr3"]["footprint"], "U9", "DDR3 #2", 30, 65),
        (MODELS["ddr3"]["footprint"], "U10", "DDR3 #3", 100, 65),
        # 4x W25Q32 (FPGA bitstream)
        (MODELS["w25q32"]["footprint"], "U11", "W25Q32 #0", 70, 30),
        (MODELS["w25q32"]["footprint"], "U12", "W25Q32 #1", 140, 30),
        (MODELS["w25q32"]["footprint"], "U13", "W25Q32 #2", 70, 80),
        (MODELS["w25q32"]["footprint"], "U14", "W25Q32 #3", 140, 80),
        # W6100 Ethernet
        (MODELS["w6100"]["footprint"], "U15", "W6100", 170, 60),
        # eMMC
        (MODELS["emmc"]["footprint"], "U16", "eMMC 8GB", 170, 80),
        # TPS54560 x2
        (MODELS["tps54560"]["footprint"], "U17", "TPS54560 3.3V", 20, 120),
        (MODELS["tps54560"]["footprint"], "U18", "TPS54560 1.1V", 50, 120),
        # AP2112K x2
        (MODELS["ap2112k"]["footprint"], "U19", "AP2112K 1.8V", 80, 120),
        (MODELS["ap2112k"]["footprint"], "U20", "AP2112K 2.5V", 100, 120),
        # Inductors
        (MODELS["inductor"]["footprint"], "L1", "4.7uH", 30, 130),
        (MODELS["inductor"]["footprint"], "L2", "4.7uH", 60, 130),
        # Oscillators
        ("Crystal:Crystal_SMD_3215-4Pin_3.2x1.5mm", "Y1", "25MHz", 170, 40),
        (MODELS["osc_diff"]["footprint"], "Y2", "100MHz", 85, 50),
        # Connectors
        (MODELS["serdes_conn"]["footprint"], "J1", "SEAF-40", 5, 75),
        (MODELS["power_conn"]["footprint"], "J2", "12V In", 190, 135),
        (MODELS["polyfuse"]["footprint"], "F1", "5A", 180, 135),
        # LEDs
        (MODELS["led"]["footprint"], "D1", "PWR", 180, 10),
        (MODELS["led"]["footprint"], "D2", "SYS", 183, 10),
        (MODELS["led"]["footprint"], "D3", "ERR", 186, 10),
        (MODELS["led"]["footprint"], "D4", "NET", 189, 10),
        # LED resistors
        (MODELS["resistor"]["footprint"], "R1", "330", 180, 14),
        (MODELS["resistor"]["footprint"], "R2", "330", 183, 14),
        (MODELS["resistor"]["footprint"], "R3", "330", 186, 14),
        (MODELS["resistor"]["footprint"], "R4", "330", 189, 14),
        # JTAG pull-up resistors
        (MODELS["resistor"]["footprint"], "R5", "10k", 170, 50),
        (MODELS["resistor"]["footprint"], "R6", "10k", 173, 50),
        (MODELS["resistor"]["footprint"], "R7", "10k", 176, 50),
        (MODELS["resistor"]["footprint"], "R8", "10k", 179, 50),
    ],
    "cap_steps": [
        {"fn": _node_pcb_ic_caps,
         "ic_positions": [(170, 30), (50, 30), (120, 30), (50, 80), (120, 80),
                          (30, 15), (100, 15), (30, 65), (100, 65),
                          (170, 60), (170, 80),
                          (20, 120), (50, 120), (80, 120), (100, 120)]},
        {"fn": _node_pcb_fill_100nf, "limit": 80},
        {"fn": _node_pcb_bulk_caps, "limit": 120},
    ],
}

_BP_SLOT_X = [20, 55, 90, 125, 160, 195, 230, 265]

BACKPLANE_PCB_DEF = {
    "width": 300, "height": 120, "layers": 4,
    "title_text": "Hydra Mesh v7.0 -- Rack Backplane", "title_x": 150, "title_y": 4,
    "mounting_holes": [(5, 5), (295, 5), (5, 115), (295, 115)],
    "footprints": (
        [(MODELS["serdes_conn"]["footprint"], f"J{i+1}", f"Slot {i+1}", sx, 60) for i, sx in enumerate(_BP_SLOT_X)]
        + [
            (MODELS["power_conn"]["footprint"], "J9", "12V In", 290, 10),
            (MODELS["polyfuse"]["footprint"], "F1", "15A", 280, 10),
            (MODELS["osc_diff"]["footprint"], "Y1", "100MHz", 150, 10),
        ]
        + [(MODELS["led"]["footprint"], f"D{i+1}", f"LED{i+1}", 10 + i * 5, 10) for i in range(4)]
        + [(MODELS["resistor"]["footprint"], f"R{i+1}", "330", 10 + i * 5, 15) for i in range(4)]
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
    print("Hydra Mesh v7.0 -- Generating KiCad 8 files...\n")

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
