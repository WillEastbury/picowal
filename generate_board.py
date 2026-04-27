#!/usr/bin/env python3
"""Hydra Mesh v11.0 -- INT8 Analogue Dataflow Engine
4x RP2354B orchestration + 12x LIFCL-17 DSP pipeline + 11x R-2R ladders
Generates KiCad 8 files: hydra_dataflow.kicad_pro/sch/pcb"""

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

def sch_header(paper="A0", title="", rev="11.0"):
    u = nuuid()
    lines = [
        "(kicad_sch",
        "  (version 20231120)",
        '  (generator "hydra_v11_gen")',
        '  (generator_version "11.0")',
        f'  (uuid "{u}")',
        f'  (paper "{paper}")',
        "  (title_block",
        f'    (title "{title}")',
        f'    (rev "{rev}")',
        '    (company "Hydra Mesh v11.0")',
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
    # eMMC 8-bit interface (2 channels for Node0 RAID-0)
    emmc0 = [("EMMC0_CLK","EM01"),("EMMC0_CMD","EM02"),
             ("EMMC0_D0","EM03"),("EMMC0_D1","EM04"),("EMMC0_D2","EM05"),("EMMC0_D3","EM06"),
             ("EMMC0_D4","EM07"),("EMMC0_D5","EM08"),("EMMC0_D6","EM09"),("EMMC0_D7","EM10"),
             ("EMMC0_RST","EM11")]
    for i, (name, num) in enumerate(emmc0):
        pins.append(_pin("bidirectional", 0, -30.48, -56.42 - i * 2.54, name, num, 1.016))
    emmc1 = [("EMMC1_CLK","EM12"),("EMMC1_CMD","EM13"),
             ("EMMC1_D0","EM14"),("EMMC1_D1","EM15"),("EMMC1_D2","EM16"),("EMMC1_D3","EM17"),
             ("EMMC1_D4","EM18"),("EMMC1_D5","EM19"),("EMMC1_D6","EM20"),("EMMC1_D7","EM21"),
             ("EMMC1_RST","EM22")]
    for i, (name, num) in enumerate(emmc1):
        pins.append(_pin("bidirectional", 0, -30.48, -84.46 - i * 2.54, name, num, 1.016))
    # Fabric QSPI link
    fabric = [("FABRIC_QSPI_SCK","FQ1"),("FABRIC_QSPI_CS","FQ2"),
              ("FABRIC_QSPI_D0","FQ3"),("FABRIC_QSPI_D1","FQ4"),
              ("FABRIC_QSPI_D2","FQ5"),("FABRIC_QSPI_D3","FQ6")]
    for i, (name, num) in enumerate(fabric):
        pins.append(_pin("bidirectional", 180, 30.48, -70.58 - i * 2.54, name, num, 1.016))
    pwr = [("VCC","P1","power_in"),("VCC_IO","P2","power_in"),
           ("GND","P3","power_in"),("GND","P4","power_in"),
           ("GND","P5","power_in"),("GND","P6","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 30.48, -86.36 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "MCU_RaspberryPi:RP2354B" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 50.8 0) (effects (font (size 1.27 1.27))))
      (property "Value" "RP2354B" (at 0 -101.6 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.5mm_EP5.6x5.6mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "RP2354B_0_1"
        (rectangle (start -27.94 48.26) (end 27.94 -99.06) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "RP2354B_1_1"
{pins_str}
      )
    )"""

def lib_symbol_ice40hx4k():
    pins = []
    pn = 1
    # 11x QSPI slave ports (6 pins each)
    for n in range(11):
        y_base = 96.52 - n * 10.16
        for j, suffix in enumerate(["SCK", "CS", "D0", "D1", "D2", "D3"]):
            pins.append(_pin("bidirectional", 0, -35.56, y_base - j * 2.54,
                             f"QSPI{n}_{suffix}", str(pn), 1.016))
            pn += 1
    # Inter-FPGA bridge (4 pins)
    bridge = [("BRIDGE_SCK", "bidirectional"), ("BRIDGE_MOSI", "bidirectional"),
              ("BRIDGE_MISO", "bidirectional"), ("BRIDGE_CS", "bidirectional")]
    for j, (name, pt) in enumerate(bridge):
        pins.append(_pin(pt, 180, 35.56, 96.52 - j * 2.54, name, str(pn), 1.016))
        pn += 1
    # Config SPI (4 pins)
    cfg = [("CFG_SCK", "output"), ("CFG_MOSI", "output"),
           ("CFG_MISO", "input"), ("CFG_CS", "output")]
    for j, (name, pt) in enumerate(cfg):
        pins.append(_pin(pt, 180, 35.56, 83.82 - j * 2.54, name, str(pn), 1.016))
        pn += 1
    # Clock
    pins.append(_pin("input", 180, 35.56, 73.66, "CLK_IN", str(pn), 1.016)); pn += 1
    pins.append(_pin("output", 180, 35.56, 71.12, "CLK_OUT", str(pn), 1.016)); pn += 1
    # LED outputs
    for j in range(3):
        pins.append(_pin("output", 180, 35.56, 66.04 - j * 2.54,
                         f"LED{j}", str(pn), 1.016))
        pn += 1
    # RMII interface (HX-A only, directly to KSZ8081 GbE PHY)
    rmii = [("RMII_TXD0", "output"), ("RMII_TXD1", "output"),
            ("RMII_TX_EN", "output"), ("RMII_RXD0", "input"),
            ("RMII_RXD1", "input"), ("RMII_CRS_DV", "input"),
            ("RMII_REF_CLK", "output")]
    for j, (name, pt) in enumerate(rmii):
        pins.append(_pin(pt, 180, 35.56, 48.26 - j * 2.54, name, str(pn), 1.016))
        pn += 1
    # CDONE / CRESET
    pins.append(_pin("output", 180, 35.56, 30.48, "CDONE", str(pn), 1.016)); pn += 1
    pins.append(_pin("input", 180, 35.56, 27.94, "CRESET", str(pn), 1.016)); pn += 1
    # Power
    pwr = [("VCC", "power_in"), ("VCCIO", "power_in"),
           ("GND", "power_in"), ("GND", "power_in"),
           ("GND", "power_in"), ("GND", "power_in")]
    for j, (name, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 35.56, 22.86 - j * 2.54, name, str(pn), 1.016))
        pn += 1
    pins_str = "\n".join(pins)
    return f"""    (symbol "FPGA_Lattice:iCE40HX4K-TQ144" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 101.6 0) (effects (font (size 1.27 1.27))))
      (property "Value" "iCE40HX4K-TQ144" (at 0 -20.32 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_QFP:TQFP-144_20x20mm_P0.5mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "iCE40HX4K-TQ144_0_1"
        (rectangle (start -33.02 99.06) (end 33.02 -17.78) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "iCE40HX4K-TQ144_1_1"
{pins_str}
      )
    )"""

def lib_symbol_tps2379():
    pins = "\n".join([
        _pin("power_in", 0, -10.16, 5.08, "VDD", "1"),
        _pin("power_in", 0, -10.16, -5.08, "VSS", "2"),
        _pin("input", 0, -10.16, 2.54, "DET", "3"),
        _pin("input", 0, -10.16, 0, "CLS", "4"),
        _pin("output", 180, 10.16, 5.08, "GATE", "5"),
        _pin("passive", 180, 10.16, 0, "DRAIN", "6"),
    ])
    return f"""    (symbol "Power_Management:TPS2379" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 10.16 0) (effects (font (size 1.27 1.27))))
      (property "Value" "TPS2379" (at 0 -10.16 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_TO_SOT_SMD:SOT-23-6" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "TPS2379_0_1"
        (rectangle (start -7.62 7.62) (end 7.62 -7.62) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "TPS2379_1_1"
{pins}
      )
    )"""

def lib_symbol_osc_25mhz():
    pins = "\n".join([
        _pin("power_in", 0, -7.62, 2.54, "VCC", "4"),
        _pin("power_in", 0, -7.62, -2.54, "GND", "2"),
        _pin("output", 180, 7.62, 2.54, "OUT", "3"),
        _pin("input", 180, 7.62, -2.54, "EN", "1"),
    ])
    return f"""    (symbol "Oscillator:OSC_25MHz" (in_bom yes) (on_board yes)
      (property "Reference" "Y" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "25MHz" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "OSC_25MHz_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "OSC_25MHz_1_1"
{pins}
      )
    )"""

def lib_symbol_emmc():
    pins = []
    sig = [("CLK","E1","input"),("CMD","E2","bidirectional"),
           ("DAT0","E3","bidirectional"),("DAT1","E4","bidirectional"),
           ("DAT2","E5","bidirectional"),("DAT3","E6","bidirectional"),
           ("DAT4","E7","bidirectional"),("DAT5","E8","bidirectional"),
           ("DAT6","E9","bidirectional"),("DAT7","E10","bidirectional"),
           ("RST","E11","input")]
    for i, (name, num, pt) in enumerate(sig):
        pins.append(_pin(pt, 0, -12.7, 12.7 - i * 2.54, name, num, 1.016))
    pwr = [("VCC","V1","power_in"),("VCCQ","V2","power_in"),("GND","V3","power_in"),("GND","V4","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 12.7, 5.08 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Memory_Flash:eMMC_32GB" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 17.78 0) (effects (font (size 1.27 1.27))))
      (property "Value" "eMMC_32GB" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_BGA:BGA-153_11.5x13.0mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "eMMC_32GB_0_1"
        (rectangle (start -10.16 15.24) (end 10.16 -15.24) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "eMMC_32GB_1_1"
{pins_str}
      )
    )"""

def lib_symbol_ksz8081():
    pins = []
    # RMII interface (directly to HX-A FPGA)
    rmii = [("TXD0","R1","input"),("TXD1","R2","input"),
            ("TX_EN","R3","input"),("RXD0","R4","output"),
            ("RXD1","R5","output"),("CRS_DV","R6","output"),
            ("REF_CLK","R7","input")]
    for i, (name, num, pt) in enumerate(rmii):
        pins.append(_pin(pt, 0, -15.24, 10.16 - i * 2.54, name, num, 1.016))
    # MDC/MDIO management
    pins.append(_pin("input", 0, -15.24, -10.16, "MDC", "M1", 1.016))
    pins.append(_pin("bidirectional", 0, -15.24, -12.7, "MDIO", "M2", 1.016))
    # Ethernet PHY pairs
    eth = [("TXP","E1","output"),("TXN","E2","output"),
           ("RXP","E3","input"),("RXN","E4","input")]
    for i, (name, num, pt) in enumerate(eth):
        pins.append(_pin(pt, 180, 15.24, 10.16 - i * 2.54, name, num, 1.016))
    # Reset + interrupt
    pins.append(_pin("input", 180, 15.24, -2.54, "RST", "RST1", 1.016))
    pins.append(_pin("output", 180, 15.24, -5.08, "INT", "INT1", 1.016))
    # Power
    pwr = [("VCC","P1","power_in"),("VDDIO","P2","power_in"),("GND","P3","power_in"),("GND","P4","power_in")]
    for i, (name, num, pt) in enumerate(pwr):
        pins.append(_pin(pt, 180, 15.24, -10.16 - i * 2.54, name, num, 1.016))
    pins_str = "\n".join(pins)
    return f"""    (symbol "Interface_Ethernet:KSZ8081RNA" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 15.24 0) (effects (font (size 1.27 1.27))))
      (property "Value" "KSZ8081RNA" (at 0 -20.32 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm_EP2.6x2.6mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "KSZ8081RNA_0_1"
        (rectangle (start -12.7 12.7) (end 12.7 -17.78) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "KSZ8081RNA_1_1"
{pins_str}
      )
    )"""

def lib_symbol_fan_header():
    pins = "\n".join([
        _pin("power_in", 180, 7.62, 2.54, "+5V", "1"),
        _pin("power_in", 180, 7.62, -2.54, "GND", "2"),
    ])
    return f"""    (symbol "Connector:Fan_2Pin" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "Fan_2Pin" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "Fan_2Pin_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "Fan_2Pin_1_1"
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

def lib_symbol_lifcl17():
    pins = []
    pn = 1
    # 6-pin QSPI fabric slave
    for j, suffix in enumerate(["SCK", "CS", "D0", "D1", "D2", "D3"]):
        pins.append(_pin("bidirectional", 0, -25.4, 30.48 - j * 2.54,
                         f"FABRIC_{suffix}", str(pn), 1.016))
        pn += 1
    # 4-pin config SPI
    for j, suffix in enumerate(["CFG_SCK", "CFG_MOSI", "CFG_MISO", "CFG_CS"]):
        pt = "input" if suffix == "CFG_MISO" else "output"
        pins.append(_pin(pt, 0, -25.4, 15.24 - j * 2.54,
                         suffix, str(pn), 1.016))
        pn += 1
    # 8-pin DAC output (pipeline out)
    for b in range(8):
        pins.append(_pin("output", 180, 25.4, 30.48 - b * 2.54,
                         f"PIPE_OUT_B{b}", str(pn), 1.016))
        pn += 1
    # 8-pin ADC input (pipeline in)
    for b in range(8):
        pins.append(_pin("input", 180, 25.4, 10.16 - b * 2.54,
                         f"PIPE_IN_B{b}", str(pn), 1.016))
        pn += 1
    # CLK_IN
    pins.append(_pin("input", 0, -25.4, 5.08, "CLK_IN", str(pn), 1.016)); pn += 1
    # 3x LED outputs
    for j in range(3):
        pins.append(_pin("output", 180, 25.4, -12.7 - j * 2.54,
                         f"LED{j}", str(pn), 1.016))
        pn += 1
    # CDONE / CRESET
    pins.append(_pin("output", 180, 25.4, -20.32, "CDONE", str(pn), 1.016)); pn += 1
    pins.append(_pin("input", 180, 25.4, -22.86, "CRESET", str(pn), 1.016)); pn += 1
    # Power
    pwr = [("VCC", "power_in"), ("VCCIO", "power_in"),
           ("GND", "power_in"), ("GND", "power_in"),
           ("GND", "power_in"), ("GND", "power_in")]
    for j, (name, pt) in enumerate(pwr):
        pins.append(_pin(pt, 0, -25.4, -7.62 - j * 2.54, name, str(pn), 1.016))
        pn += 1
    pins_str = "\n".join(pins)
    return f"""    (symbol "FPGA_Lattice:LIFCL-17-QFN72" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 35.56 0) (effects (font (size 1.27 1.27))))
      (property "Value" "LIFCL-17" (at 0 -27.94 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Package_DFN_QFN:QFN-72-1EP_9x9mm_P0.4mm" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "LIFCL-17-QFN72_0_1"
        (rectangle (start -22.86 33.02) (end 22.86 -25.4) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "LIFCL-17-QFN72_1_1"
{pins_str}
      )
    )"""

def lib_symbol_r2r_ladder():
    pins = []
    pn = 1
    # 8 input pins (left side)
    for b in range(8):
        pins.append(_pin("input", 0, -10.16, 8.89 - b * 2.54,
                         f"BIT{b}", str(pn), 1.016))
        pn += 1
    # VOUT (right side)
    pins.append(_pin("output", 180, 10.16, 2.54, "VOUT", str(pn), 1.016)); pn += 1
    # Power
    pins.append(_pin("power_in", 0, -10.16, -12.7, "VCC_REF", str(pn), 1.016)); pn += 1
    pins.append(_pin("power_in", 0, -10.16, -15.24, "GND", str(pn), 1.016)); pn += 1
    pins_str = "\n".join(pins)
    return f"""    (symbol "Analog_DAC:R2R_Ladder_8bit" (in_bom yes) (on_board yes)
      (property "Reference" "RN" (at 0 12.7 0) (effects (font (size 1.27 1.27))))
      (property "Value" "R2R_8bit" (at 0 -17.78 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Resistor_SMD:R2R_8bit_0402_array" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "R2R_Ladder_8bit_0_1"
        (rectangle (start -7.62 10.16) (end 7.62 -15.24) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "R2R_Ladder_8bit_1_1"
{pins_str}
      )
    )"""

def lib_symbol_power_conn():
    pins = "\n".join([
        _pin("power_out", 180, 7.62, 2.54, "VCC_48V_POE", "1"),
        _pin("power_out", 180, 7.62, -2.54, "GND", "2"),
    ])
    return f"""    (symbol "Connector:RJ45_MagJack" (in_bom yes) (on_board yes)
      (property "Reference" "J" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
      (property "Value" "RJ45_MagJack" (at 0 -7.62 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "Connector_RJ:RJ45_MagJack" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "RJ45_MagJack_0_1"
        (rectangle (start -5.08 5.08) (end 5.08 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "RJ45_MagJack_1_1"
{pins}
      )
    )"""

# == PCB Helpers ==

def pcb_header(board_w, board_h, layers=4):
    u = nuuid()
    layer_defs = """    (0 "F.Cu" signal)
    (1 "In1.Cu" signal)
    (2 "In2.Cu" signal)
    (31 "B.Cu" signal)"""
    return f"""(kicad_pcb
  (version 20240108)
  (generator "hydra_v11_gen")
  (generator_version "11.0")
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
    "ice40hx4k": {
        "lib_id": "FPGA_Lattice:iCE40HX4K-TQ144",
        "lib_symbol_fn": lib_symbol_ice40hx4k,
        "footprint": "Package_QFP:TQFP-144_20x20mm_P0.5mm",
    },
    "lifcl17": {
        "lib_id": "FPGA_Lattice:LIFCL-17-QFN72",
        "lib_symbol_fn": lib_symbol_lifcl17,
        "footprint": "Package_DFN_QFN:QFN-72-1EP_9x9mm_P0.4mm",
    },
    "r2r_ladder": {
        "lib_id": "Analog_DAC:R2R_Ladder_8bit",
        "lib_symbol_fn": lib_symbol_r2r_ladder,
        "footprint": "Resistor_SMD:R2R_8bit_0402_array",
    },
    "ksz8081": {
        "lib_id": "Interface_Ethernet:KSZ8081RNA",
        "lib_symbol_fn": lib_symbol_ksz8081,
        "footprint": "Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm_EP2.6x2.6mm",
    },
    "fan_header": {
        "lib_id": "Connector:Fan_2Pin",
        "lib_symbol_fn": lib_symbol_fan_header,
        "footprint": "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
    },
    "emmc": {
        "lib_id": "Memory_Flash:eMMC_32GB",
        "lib_symbol_fn": lib_symbol_emmc,
        "footprint": "Package_BGA:BGA-153_11.5x13.0mm",
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
    "tps2379": {
        "lib_id": "Power_Management:TPS2379",
        "lib_symbol_fn": lib_symbol_tps2379,
        "footprint": "Package_TO_SOT_SMD:SOT-23-6",
    },
    "osc_25mhz": {
        "lib_id": "Oscillator:OSC_25MHz",
        "lib_symbol_fn": lib_symbol_osc_25mhz,
        "footprint": "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_2.5x2.0mm",
    },
    "power_conn": {
        "lib_id": "Connector:RJ45_MagJack",
        "lib_symbol_fn": lib_symbol_power_conn,
        "footprint": "Connector_RJ:RJ45_MagJack",
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

def _handle_node_cluster(ctx, sp):
    """Place N RP2354B nodes in a grid with their flash chips and fabric QSPI links."""
    node_start = sp["node_start"]
    node_end = sp["node_end"]
    fabric_ref = sp["fabric_ref"]  # "HXA" or "HXB"
    rows = sp["rows"]
    cols = sp["cols"]
    x_base = sp["x_base"]
    y_base = sp["y_base"]
    x_step = sp["x_step"]
    y_step = sp["y_step"]
    rp_ref_start = sp["rp_ref_start"]
    flash_ref_start = sp["flash_ref_start"]

    for idx, node_num in enumerate(range(node_start, node_end + 1)):
        row = idx // cols
        col = idx % cols
        nx = x_base + col * x_step
        ny = y_base + row * y_step

        rp_ref = f"U{rp_ref_start + idx}"
        flash_ref = f"U{flash_ref_start + idx}"

        # RP2354B
        ctx["s"] += place_symbol("MCU_RaspberryPi:RP2354B", rp_ref,
                                  f"RP2354B Node{node_num} @500MHz", nx, ny)
        # W25Q128 flash (directly on RP, not through fabric)
        ctx["s"] += place_symbol("Memory_Flash:W25Q128JV", flash_ref,
                                  f"W25Q128 N{node_num}", nx + 40, ny - 20)

        # QSPI flash labels (RP ↔ flash)
        qspi_sigs = ["SCK", "CS", "D0", "D1", "D2", "D3"]
        for j, sig in enumerate(qspi_sigs):
            label = f"N{node_num}_QSPI_{sig}"
            ctx["s"] += place_label(label, nx + 33, ny - 46.74 - j * 2.54)
            if j < 2:
                flash_y = ny - 20 + (3.81 if j == 0 else 1.27)
                ctx["s"] += place_label(label, nx + 40 + 13, flash_y)
            else:
                flash_y = ny - 20 + 3.81 - (j - 2) * 2.54
                ctx["s"] += place_label(label, nx + 40 - 13, flash_y)

        # Fabric QSPI labels (RP ↔ HX fabric)
        fabric_sigs = ["SCK", "CS", "D0", "D1", "D2", "D3"]
        for j, sig in enumerate(fabric_sigs):
            ctx["s"] += place_global_label(f"N{node_num}_FABRIC_{sig}",
                                            nx + 33, ny - 70.58 - j * 2.54, 0, "bidirectional")

        # SWD debug labels
        ctx["s"] += place_global_label(f"N{node_num}_SWD_CLK", nx + 33, ny - 62.74, 0, "bidirectional")
        ctx["s"] += place_global_label(f"N{node_num}_SWD_DIO", nx + 33, ny - 65.28, 0, "bidirectional")

        # Power
        ctx["s"] += place_global_label("VCC_3V3", nx + 33, ny - 86.36, 0, "input")
        ctx["s"] += place_global_label("VCC_3V3", nx + 33, ny - 88.9, 0, "input")
        ctx["s"] += place_global_label("GND", nx + 33, ny - 91.44, 0, "input")
        ctx["s"] += place_global_label("GND", nx + 33, ny - 93.98, 0, "input")

        # Flash power
        ctx["s"] += place_global_label("VCC_3V3", nx + 40 + 13, ny - 20 - 3.81, 0, "input")
        ctx["s"] += place_global_label("GND", nx + 40 - 13, ny - 20 - 3.81, 180, "input")

        # Per-node decoupling (2 caps)
        for dc in range(2):
            ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF",
                                      nx + 40 + dc * 7, ny + 30)
            ctx["s"] += place_global_label("VCC_3V3", nx + 40 + dc * 7, ny + 32.54, 0, "input")
            ctx["s"] += place_global_label("GND", nx + 40 + dc * 7, ny + 27.46, 0, "input")
            ctx["c_idx"] += 1

def _handle_dataflow_pipeline(ctx, sp):
    """Place 12 LIFCL-17 engines in a pipeline chain with R-2R ladders between stages."""
    x_base = sp["x_base"]
    y_base = sp["y_base"]
    x_step = sp["x_step"]
    y_step = sp["y_step"]
    cols = sp.get("cols", 6)
    lifcl_ref_start = sp["lifcl_ref_start"]   # U9
    flash_ref_start = sp["flash_ref_start"]    # U23
    ladder_start = sp.get("ladder_start", 0)   # first ladder index
    r_idx_start = sp.get("r_idx_start", 9)     # R9 onwards for R-2R resistors

    for eng in range(12):
        row = eng // cols
        col = eng % cols
        ex = x_base + col * x_step
        ey = y_base + row * y_step

        u_ref = f"U{lifcl_ref_start + eng}"
        flash_ref = f"U{flash_ref_start + eng}"

        # LIFCL-17 engine
        ctx["s"] += place_symbol("FPGA_Lattice:LIFCL-17-QFN72", u_ref,
                                  f"LIFCL-17 DSP{eng} @200MHz", ex, ey)

        # Config flash
        ctx["s"] += place_symbol("Memory_Flash:W25Q32JV", flash_ref,
                                  f"W25Q32 DSP{eng} Cfg", ex + 35, ey - 15)

        # Config SPI labels (LIFCL ↔ flash)
        for j, suffix in enumerate(["SCK", "MOSI", "MISO", "CS"]):
            label = f"DSP{eng}_CFG_{suffix}"
            ctx["s"] += place_label(label, ex - 28, ey + 15.24 - j * 2.54)
            if j < 2:
                ctx["s"] += place_label(label, ex + 35 + 13, ey - 15 + (3.81 if j == 0 else 1.27))
            else:
                ctx["s"] += place_label(label, ex + 35 - 13, ey - 15 + 3.81 - (j - 2) * 2.54)

        # Flash power
        ctx["s"] += place_global_label("VCC_3V3", ex + 35 + 13, ey - 15 - 3.81, 0, "input")
        ctx["s"] += place_global_label("GND", ex + 35 - 13, ey - 15 - 3.81, 180, "input")

        # Fabric QSPI labels (LIFCL ↔ HX switch)
        for j, suffix in enumerate(["SCK", "CS", "D0", "D1", "D2", "D3"]):
            ctx["s"] += place_global_label(f"DSP{eng}_FABRIC_{suffix}",
                                            ex - 28, ey + 30.48 - j * 2.54, 180, "bidirectional")

        # Clock
        ctx["s"] += place_global_label("CLK_25MHZ", ex - 28, ey + 5.08, 180, "input")

        # Pipeline output labels (DAC)
        for b in range(8):
            ctx["s"] += place_label(f"PIPE_{eng}_DAC_B{b}", ex + 28, ey + 30.48 - b * 2.54)

        # Pipeline input labels (DIN) — stage 0 has no input from previous ladder
        if eng > 0:
            for b in range(8):
                ctx["s"] += place_label(f"PIPE_{eng}_DIN_B{b}", ex + 28, ey + 10.16 - b * 2.54)

        # Power
        ctx["s"] += place_global_label("VCC_3V3", ex - 28, ey - 7.62, 180, "input")
        ctx["s"] += place_global_label("VCC_3V3", ex - 28, ey - 10.16, 180, "input")
        ctx["s"] += place_global_label("GND", ex - 28, ey - 12.7, 180, "input")
        ctx["s"] += place_global_label("GND", ex - 28, ey - 15.24, 180, "input")
        ctx["s"] += place_global_label("GND", ex - 28, ey - 17.78, 180, "input")
        ctx["s"] += place_global_label("GND", ex - 28, ey - 20.32, 180, "input")

        # Per-engine decoupling (2 caps)
        for dc in range(2):
            ctx["s"] += place_symbol("Device:C", f"C{ctx['c_idx']}", "100nF",
                                      ex + 35 + dc * 7, ey + 25)
            ctx["s"] += place_global_label("VCC_3V3", ex + 35 + dc * 7, ey + 27.54, 0, "input")
            ctx["s"] += place_global_label("GND", ex + 35 + dc * 7, ey + 22.46, 0, "input")
            ctx["c_idx"] += 1

    # R-2R ladders between adjacent stages (11 ladders)
    rn_idx = 1
    for stage in range(11):
        src_eng = stage
        dst_eng = stage + 1
        src_col = src_eng % cols
        src_row = src_eng // cols
        dst_col = dst_eng % cols
        dst_row = dst_eng // cols

        # Place ladder symbol between source and destination
        lx = x_base + (src_col + 0.5) * x_step + 15
        if src_row != dst_row:
            # Wrapping to next row — place ladder at end of row
            lx = x_base + (cols - 0.5) * x_step + 25
        ly = y_base + src_row * y_step + 45

        ctx["s"] += place_symbol("Analog_DAC:R2R_Ladder_8bit", f"RN{rn_idx}",
                                  f"R2R Stage {stage}→{stage+1}", lx, ly)
        rn_idx += 1

        # Wire ladder inputs from source DAC outputs
        for b in range(8):
            ctx["s"] += place_label(f"PIPE_{src_eng}_DAC_B{b}", lx - 13, ly + 8.89 - b * 2.54)

        # Wire ladder VOUT to destination DIN (via labels)
        for b in range(8):
            ctx["s"] += place_label(f"PIPE_{dst_eng}_DIN_B{b}", lx + 13, ly + 2.54)

        # Ladder power
        ctx["s"] += place_global_label("VCC_3V3", lx - 13, ly - 12.7, 180, "input")
        ctx["s"] += place_global_label("GND", lx - 13, ly - 15.24, 180, "input")

    # Place precision resistors for BOM (R9-R184: 176 resistors)
    r_label_x = sp.get("r_label_x", x_base)
    r_label_y = sp.get("r_label_y", y_base + 2 * y_step + 80)
    ctx["s"] += place_text("R-2R Precision Resistors: R9-R184 (176x 0.1% 0402)",
                           r_label_x, r_label_y, 1.5)
    for ladder in range(11):
        for rr in range(16):
            r_num = r_idx_start + ladder * 16 + rr
            val = "10k 0.1%" if rr % 2 == 0 else "20k 0.1%"
            rx = r_label_x + (rr % 16) * 6
            ry = r_label_y + 5 + ladder * 4
            ctx["s"] += place_symbol("Device:R", f"R{r_num}", val, rx, ry)
            ctx["r_idx"] = max(ctx["r_idx"], r_num + 1)

SPLICE_HANDLERS = {
    "text": _handle_text,
    "symbol": _handle_symbol,
    "bus_labels": _handle_bus_labels,
    "global_labels": _handle_global_labels,
    "global_label_fan": _handle_global_label_fan,
    "wire": _handle_wire,
    "decoupling": _handle_decoupling,
    "pull_ups": _handle_pull_ups,
    "regulator_block": _handle_regulator_block,
    "bulk_caps": _handle_bulk_caps,
    "fill_caps": _handle_fill_caps,
    "led_block": _handle_led_block,
    "node_led_pair": _handle_node_led_pair,
    "node_cluster": _handle_node_cluster,
    "dataflow_pipeline": _handle_dataflow_pipeline,
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
# PCB CAP STEPS
# ============================================================

def _pcb_ic_caps(step, ctx):
    ic_positions = step["ic_positions"]
    for ix, iy in ic_positions:
        for dc in range(2):
            ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                       f"C{ctx['c_idx']}", "100nF", ix - 8 + dc * 3, iy + 12)
            ctx["c_idx"] += 1

def _pcb_fill_100nf(step, ctx):
    limit = step.get("limit", 100)
    while ctx["c_idx"] <= limit:
        col = (ctx["c_idx"] - 1) % 20
        row = (ctx["c_idx"] - 1) // 20
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0402_1005Metric",
                                   f"C{ctx['c_idx']}", "100nF", 10 + col * 7, 85 + row * 3)
        ctx["c_idx"] += 1

def _pcb_bulk_caps(step, ctx):
    limit = step.get("limit", 160)
    start = ctx["c_idx"]
    while ctx["c_idx"] <= limit:
        col = (ctx["c_idx"] - start) % 12
        row = (ctx["c_idx"] - start) // 12
        ctx["s"] += pcb_footprint("Capacitor_SMD:C_0805_2012Metric",
                                   f"C{ctx['c_idx']}", "10uF", 15 + col * 12, 92 + row * 4)
        ctx["c_idx"] += 1

# ============================================================
# BOARD DEFINITION -- Hydra Mesh v11.0 INT8 Analogue Dataflow
# ============================================================

NODE_BOARD = {
    "project_name": "hydra_dataflow",
    "header": {
        "paper": "A0",
        "title": "Hydra Mesh v11.0 -- INT8 Analogue Dataflow Engine -- 12x LIFCL-17 + 4x RP2354B",
        "rev": "11.0",
    },
    "lib_symbol_order": [
        "rp2354b", "ice40hx4k", "lifcl17", "r2r_ladder", "ksz8081", "emmc",
        "w25q128", "w25q32", "tps54560", "ap2112k", "tps2379", "osc_25mhz",
        "power_conn", "polyfuse", "fan_header",
        "resistor", "cap", "cap_0805", "cap_1206", "led", "inductor",
    ],
    "splices": [
        {"type": "text", "text": "HYDRA MESH v11.0 -- INT8 Analogue Dataflow Engine",
         "x": 400, "y": 10, "size": 3.0},
        {"type": "text",
         "text": "12x LIFCL-17 = 384 DSP MACs, 155.6 GOPS peak (INT8), ~140 GOPS sustained, ~180ns latency, ~5.5M inf/s, 14W PoE",
         "x": 400, "y": 18, "size": 1.5},

        # ======================================================
        # Section 1: Orchestration Nodes (top-left)
        # 4x RP2354B + W25Q128 + SWD debug
        # Node0: bus master, eMMC, GbE, model loading
        # Nodes 1-3: pre/post-processing, data marshalling
        # ======================================================
        {"type": "text", "text": "Section 1: Orchestration Nodes -- 4x RP2354B + eMMC + Fabric Links",
         "x": 120, "y": 30, "size": 2.0},

        # U1: Node0 RP2354B (bus master)
        {"type": "symbol", "model": "rp2354b", "ref": "U1", "value": "RP2354B Node0 @500MHz (Bus Master)", "x": 80, "y": 100},
        # U5: W25Q128 flash for Node0
        {"type": "symbol", "model": "w25q128", "ref": "U5", "value": "W25Q128 N0 Flash", "x": 140, "y": 55},
        {"type": "bus_labels", "prefix": "N0_QSPI_", "signals": ["SCK","CS","D0","D1","D2","D3"],
         "a_x": 113, "a_y_base": 53.26, "a_y_step": -2.54,
         "b_x": 150, "b_y_base": 58.81, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 153, 51.19, 0, "input"),
            ("GND", 127, 51.19, 180, "input"),
        ]},
        # Node0 fabric QSPI link to HX-A
        {"type": "global_label_fan", "signals": [
            "RP0_FABRIC_SCK","RP0_FABRIC_CS","RP0_FABRIC_D0","RP0_FABRIC_D1","RP0_FABRIC_D2","RP0_FABRIC_D3"],
         "x": 113, "y_base": 29.42, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # Node0 power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 113, 13.64, 0, "input"),
            ("VCC_3V3", 113, 11.1, 0, "input"),
            ("GND", 113, 8.56, 0, "input"),
            ("GND", 113, 6.02, 0, "input"),
        ]},
        # Node0 SWD
        {"type": "global_label_fan", "signals": ["N0_SWD_CLK","N0_SWD_DIO"],
         "x": 113, "y_base": 37.26, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # Node0 decoupling
        {"type": "decoupling", "x_base": 130, "y": 70, "count": 2, "x_step": 7,
         "vcc_label": "VCC_3V3", "bulk": True, "bulk_x": 130, "bulk_y": 80},

        # U37: eMMC #0 (32GB, 8-bit) on Node0
        {"type": "symbol", "model": "emmc", "ref": "U37", "value": "eMMC #0 32GB", "x": 80, "y": 230},
        {"type": "bus_labels", "prefix": "EMMC0_", "signals": ["CLK","CMD","D0","D1","D2","D3","D4","D5","D6","D7","RST"],
         "a_x": 47, "a_y_base": 43.58, "a_y_step": -2.54,
         "b_x": 65, "b_y_base": 242.7, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 95, 235.08, 0, "input"),
            ("VCC_1V8", 95, 232.54, 0, "input"),
            ("GND", 95, 230.0, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 105, "y": 230, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # U38: eMMC #1 (32GB, 8-bit) on Node0 (RAID-0 pair)
        {"type": "symbol", "model": "emmc", "ref": "U38", "value": "eMMC #1 32GB", "x": 80, "y": 270},
        {"type": "bus_labels", "prefix": "EMMC1_", "signals": ["CLK","CMD","D0","D1","D2","D3","D4","D5","D6","D7","RST"],
         "a_x": 47, "a_y_base": 15.54, "a_y_step": -2.54,
         "b_x": 65, "b_y_base": 282.7, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 95, 275.08, 0, "input"),
            ("VCC_1V8", 95, 272.54, 0, "input"),
            ("GND", 95, 270.0, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 105, "y": 270, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # U2: Node1 RP2354B (pre-processing)
        {"type": "symbol", "model": "rp2354b", "ref": "U2", "value": "RP2354B Node1 @500MHz", "x": 250, "y": 100},
        {"type": "symbol", "model": "w25q128", "ref": "U6", "value": "W25Q128 N1 Flash", "x": 310, "y": 55},
        {"type": "bus_labels", "prefix": "N1_QSPI_", "signals": ["SCK","CS","D0","D1","D2","D3"],
         "a_x": 283, "a_y_base": 53.26, "a_y_step": -2.54,
         "b_x": 320, "b_y_base": 58.81, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [("VCC_3V3", 323, 51.19, 0, "input"), ("GND", 297, 51.19, 180, "input")]},
        {"type": "global_label_fan", "signals": [
            "RP1_FABRIC_SCK","RP1_FABRIC_CS","RP1_FABRIC_D0","RP1_FABRIC_D1","RP1_FABRIC_D2","RP1_FABRIC_D3"],
         "x": 283, "y_base": 29.42, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 283, 13.64, 0, "input"), ("VCC_3V3", 283, 11.1, 0, "input"),
            ("GND", 283, 8.56, 0, "input"), ("GND", 283, 6.02, 0, "input"),
        ]},
        {"type": "global_label_fan", "signals": ["N1_SWD_CLK","N1_SWD_DIO"],
         "x": 283, "y_base": 37.26, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "decoupling", "x_base": 300, "y": 70, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # U3: Node2 RP2354B (post-processing)
        {"type": "symbol", "model": "rp2354b", "ref": "U3", "value": "RP2354B Node2 @500MHz", "x": 420, "y": 100},
        {"type": "symbol", "model": "w25q128", "ref": "U7", "value": "W25Q128 N2 Flash", "x": 480, "y": 55},
        {"type": "bus_labels", "prefix": "N2_QSPI_", "signals": ["SCK","CS","D0","D1","D2","D3"],
         "a_x": 453, "a_y_base": 53.26, "a_y_step": -2.54,
         "b_x": 490, "b_y_base": 58.81, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [("VCC_3V3", 493, 51.19, 0, "input"), ("GND", 467, 51.19, 180, "input")]},
        {"type": "global_label_fan", "signals": [
            "RP2_FABRIC_SCK","RP2_FABRIC_CS","RP2_FABRIC_D0","RP2_FABRIC_D1","RP2_FABRIC_D2","RP2_FABRIC_D3"],
         "x": 453, "y_base": 29.42, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 453, 13.64, 0, "input"), ("VCC_3V3", 453, 11.1, 0, "input"),
            ("GND", 453, 8.56, 0, "input"), ("GND", 453, 6.02, 0, "input"),
        ]},
        {"type": "global_label_fan", "signals": ["N2_SWD_CLK","N2_SWD_DIO"],
         "x": 453, "y_base": 37.26, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "decoupling", "x_base": 470, "y": 70, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # U4: Node3 RP2354B (data marshalling)
        {"type": "symbol", "model": "rp2354b", "ref": "U4", "value": "RP2354B Node3 @500MHz", "x": 590, "y": 100},
        {"type": "symbol", "model": "w25q128", "ref": "U8", "value": "W25Q128 N3 Flash", "x": 650, "y": 55},
        {"type": "bus_labels", "prefix": "N3_QSPI_", "signals": ["SCK","CS","D0","D1","D2","D3"],
         "a_x": 623, "a_y_base": 53.26, "a_y_step": -2.54,
         "b_x": 660, "b_y_base": 58.81, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [("VCC_3V3", 663, 51.19, 0, "input"), ("GND", 637, 51.19, 180, "input")]},
        {"type": "global_label_fan", "signals": [
            "RP3_FABRIC_SCK","RP3_FABRIC_CS","RP3_FABRIC_D0","RP3_FABRIC_D1","RP3_FABRIC_D2","RP3_FABRIC_D3"],
         "x": 623, "y_base": 29.42, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 623, 13.64, 0, "input"), ("VCC_3V3", 623, 11.1, 0, "input"),
            ("GND", 623, 8.56, 0, "input"), ("GND", 623, 6.02, 0, "input"),
        ]},
        {"type": "global_label_fan", "signals": ["N3_SWD_CLK","N3_SWD_DIO"],
         "x": 623, "y_base": 37.26, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "decoupling", "x_base": 640, "y": 70, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # ======================================================
        # Section 2: INT8 Analogue Dataflow Pipeline (center, large)
        # 12x LIFCL-17 in 2 rows of 6 + 11x R-2R ladders
        # ======================================================
        {"type": "text", "text": "Section 2: INT8 Analogue Dataflow Pipeline -- 12 stages, ~180ns latency",
         "x": 350, "y": 300, "size": 2.0},
        {"type": "text",
         "text": "384 DSP MACs | 2.16MB weight BRAM | R-2R Kirchhoff current summing | Flash ADC digitize",
         "x": 350, "y": 308, "size": 1.5},

        # U9-U20: 12x LIFCL-17 engines in pipeline + U23-U34: config flash
        {"type": "dataflow_pipeline",
         "x_base": 100, "y_base": 350,
         "x_step": 110, "y_step": 160,
         "cols": 6,
         "lifcl_ref_start": 9,
         "flash_ref_start": 23,
         "r_idx_start": 9,
         "r_label_x": 100, "r_label_y": 700,
        },

        # ======================================================
        # Section 3: SPI Fabric Switches (left)
        # 2x iCE40HX4K + config flash
        # ======================================================
        {"type": "text", "text": "Section 3: SPI Fabric Switches -- 2x iCE40HX4K + Config Flash",
         "x": 120, "y": 760, "size": 2.0},

        # U21: iCE40HX4K-A (RP nodes 0-1 + LIFCL engines 0-5 + KSZ8081 RMII)
        {"type": "symbol", "model": "ice40hx4k", "ref": "U21", "value": "iCE40HX4K-A (RP0-1 + DSP0-5 + RMII)", "x": 80, "y": 880},
        # HX-A QSPI slave ports: 2 RP nodes + 6 LIFCL engines = 8 ports (uses 8 of 11 slots)
        {"type": "global_label_fan",
         "signals": [f"RP{n}_FABRIC_{s}" for n in range(2) for s in ["SCK","CS","D0","D1","D2","D3"]]
                  + [f"DSP{n}_FABRIC_{s}" for n in range(6) for s in ["SCK","CS","D0","D1","D2","D3"]],
         "x": 42, "y_base": 976.52, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # HX-A bridge to HX-B
        {"type": "global_label_fan",
         "signals": ["HX_BRIDGE_SCK","HX_BRIDGE_MOSI","HX_BRIDGE_MISO","HX_BRIDGE_CS"],
         "x": 118, "y_base": 976.52, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # HX-A config SPI
        {"type": "global_label_fan",
         "signals": ["HXA_CFG_SCK","HXA_CFG_MOSI","HXA_CFG_MISO","HXA_CFG_CS"],
         "x": 118, "y_base": 963.82, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # HX-A clock
        {"type": "global_labels", "labels": [
            ("CLK_25MHZ", 118, 953.66, 0, "input"),
            ("HXA_CLK_OUT", 118, 951.12, 0, "output"),
        ]},
        # HX-A LEDs
        {"type": "global_labels", "labels": [
            ("LED_HXA_0", 118, 946.04, 0, "output"),
            ("LED_HXA_1", 118, 943.5, 0, "output"),
            ("LED_HXA_2", 118, 940.96, 0, "output"),
        ]},
        # HX-A RMII interface → KSZ8081 GbE PHY
        {"type": "global_label_fan",
         "signals": ["RMII_TXD0","RMII_TXD1","RMII_TX_EN","RMII_RXD0","RMII_RXD1","RMII_CRS_DV","RMII_REF_CLK"],
         "x": 118, "y_base": 935.88, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # HX-A CDONE/CRESET
        {"type": "global_labels", "labels": [
            ("HXA_CDONE", 118, 910.48, 0, "output"),
            ("HXA_CRESET", 118, 907.94, 0, "input"),
        ]},
        # HX-A power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 118, 902.8, 0, "input"),
            ("VCC_3V3", 118, 900.26, 0, "input"),
            ("GND", 118, 897.72, 0, "input"),
            ("GND", 118, 895.18, 0, "input"),
            ("GND", 118, 892.64, 0, "input"),
            ("GND", 118, 890.1, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 130, "y": 900, "count": 3, "x_step": 7, "vcc_label": "VCC_3V3",
         "bulk": True, "bulk_x": 130, "bulk_y": 910},

        # U35: W25Q32 config flash for HX-A
        {"type": "symbol", "model": "w25q32", "ref": "U35", "value": "W25Q32 HX-A Cfg", "x": 160, "y": 880},
        {"type": "bus_labels", "prefix": "HXA_CFG_", "signals": ["SCK","MOSI","MISO","CS"],
         "a_x": 118, "a_y_base": 963.82, "a_y_step": -2.54,
         "b_x": 173, "b_y_base": 881.27, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 173, 876.19, 0, "input"),
            ("GND", 147, 876.19, 180, "input"),
        ]},

        # U22: iCE40HX4K-B (RP nodes 2-3 + LIFCL engines 6-11)
        {"type": "symbol", "model": "ice40hx4k", "ref": "U22", "value": "iCE40HX4K-B (RP2-3 + DSP6-11)", "x": 80, "y": 1080},
        {"type": "global_label_fan",
         "signals": [f"RP{n}_FABRIC_{s}" for n in range(2, 4) for s in ["SCK","CS","D0","D1","D2","D3"]]
                  + [f"DSP{n}_FABRIC_{s}" for n in range(6, 12) for s in ["SCK","CS","D0","D1","D2","D3"]],
         "x": 42, "y_base": 1176.52, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        # HX-B bridge to HX-A
        {"type": "global_label_fan",
         "signals": ["HX_BRIDGE_SCK","HX_BRIDGE_MOSI","HX_BRIDGE_MISO","HX_BRIDGE_CS"],
         "x": 118, "y_base": 1176.52, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # HX-B config SPI
        {"type": "global_label_fan",
         "signals": ["HXB_CFG_SCK","HXB_CFG_MOSI","HXB_CFG_MISO","HXB_CFG_CS"],
         "x": 118, "y_base": 1163.82, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        # HX-B clock
        {"type": "global_labels", "labels": [
            ("CLK_25MHZ", 118, 1153.66, 0, "input"),
            ("HXB_CLK_OUT", 118, 1151.12, 0, "output"),
        ]},
        # HX-B LEDs
        {"type": "global_labels", "labels": [
            ("LED_HXB_0", 118, 1146.04, 0, "output"),
            ("LED_HXB_1", 118, 1143.5, 0, "output"),
            ("LED_HXB_2", 118, 1140.96, 0, "output"),
        ]},
        # HX-B CDONE/CRESET
        {"type": "global_labels", "labels": [
            ("HXB_CDONE", 118, 1110.48, 0, "output"),
            ("HXB_CRESET", 118, 1107.94, 0, "input"),
        ]},
        # HX-B power
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 118, 1102.8, 0, "input"),
            ("VCC_3V3", 118, 1100.26, 0, "input"),
            ("GND", 118, 1097.72, 0, "input"),
            ("GND", 118, 1095.18, 0, "input"),
            ("GND", 118, 1092.64, 0, "input"),
            ("GND", 118, 1090.1, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 130, "y": 1100, "count": 3, "x_step": 7, "vcc_label": "VCC_3V3",
         "bulk": True, "bulk_x": 130, "bulk_y": 1110},

        # U36: W25Q32 config flash for HX-B
        {"type": "symbol", "model": "w25q32", "ref": "U36", "value": "W25Q32 HX-B Cfg", "x": 160, "y": 1080},
        {"type": "bus_labels", "prefix": "HXB_CFG_", "signals": ["SCK","MOSI","MISO","CS"],
         "a_x": 118, "a_y_base": 1163.82, "a_y_step": -2.54,
         "b_x": 173, "b_y_base": 1081.27, "b_y_step": -2.54},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 173, 1076.19, 0, "input"),
            ("GND", 147, 1076.19, 180, "input"),
        ]},

        # ======================================================
        # Section 4: Networking (top-right)
        # KSZ8081 GbE PHY (RMII via HX-A) + RJ45 + PoE
        # ======================================================
        {"type": "text", "text": "Section 4: Networking -- KSZ8081 GbE PHY (RMII via HX-A) + PoE",
         "x": 600, "y": 30, "size": 2.0},

        # U39: KSZ8081RNA GbE PHY
        {"type": "symbol", "model": "ksz8081", "ref": "U39", "value": "KSZ8081RNA GbE", "x": 650, "y": 315},
        {"type": "global_label_fan", "signals": [
            "RMII_TXD0","RMII_TXD1","RMII_TX_EN","RMII_RXD0","RMII_RXD1","RMII_CRS_DV","RMII_REF_CLK"],
         "x": 632, "y_base": 325.16, "y_step": -2.54, "angle": 180, "shape": "bidirectional"},
        {"type": "global_label_fan", "signals": ["ETH_TXP","ETH_TXN","ETH_RXP","ETH_RXN"],
         "x": 668, "y_base": 325.16, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "global_label_fan", "signals": ["PHY_RST","PHY_INT"],
         "x": 668, "y_base": 312.46, "y_step": -2.54, "angle": 0, "shape": "bidirectional"},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 668, 304.8, 0, "input"),
            ("VCC_3V3", 668, 302.26, 0, "input"),
            ("GND", 668, 299.72, 0, "input"),
            ("GND", 668, 297.18, 0, "input"),
        ]},
        {"type": "decoupling", "x_base": 680, "y": 315, "count": 2, "x_step": 7, "vcc_label": "VCC_3V3"},

        # J1: RJ45 MagJack with PoE center-tap
        {"type": "symbol", "model": "power_conn", "ref": "J1", "value": "RJ45 MagJack PoE", "x": 700, "y": 315},
        {"type": "global_labels", "labels": [
            ("VCC_48V_POE", 710, 317.54, 0, "output"),
            ("GND", 710, 312.46, 0, "input"),
        ]},

        # U40: TPS2379 PoE PD controller (802.3at 25W)
        {"type": "symbol", "model": "tps2379", "ref": "U40", "value": "TPS2379 PoE PD 25W", "x": 700, "y": 345},
        {"type": "global_labels", "labels": [
            ("VCC_48V_POE", 687, 350.08, 180, "input"),
            ("GND", 687, 339.92, 180, "input"),
        ]},
        {"type": "decoupling", "x_base": 720, "y": 345, "count": 1, "x_step": 7, "vcc_label": "VCC_48V_POE"},

        # ======================================================
        # Section 5: Power Supply (bottom)
        # PoE → polyfuse → TPS54560 3.3V + TPS54560 5V (fan) → AP2112K 1.8V
        # ======================================================
        {"type": "text", "text": "Section 5: Power Supply -- PoE 48V → 3.3V/5A + 5V/2A (fan) → 1.8V (eMMC VCCQ)",
         "x": 300, "y": 1200, "size": 2.0},

        # F1: Polyfuse
        {"type": "symbol", "model": "polyfuse", "ref": "F1", "value": "1A Polyfuse", "x": 200, "y": 1222},
        {"type": "global_labels", "labels": [
            ("VCC_48V_POE", 192, 1222, 180, "output"),
            ("VCC_48V_FUSED", 208, 1222, 0, "output"),
        ]},

        # U41: TPS54560 48V→3.3V/5A
        {"type": "regulator_block", "model": "tps54560", "ref": "U41", "value": "TPS54560 48V→3.3V/5A",
         "x": 260, "y": 1230,
         "globals": [
             ("VCC_48V_FUSED", 245, 1237.62, 180, "input"),
             ("VCC_3V3", 275, 1237.62, 0, "output"),
             ("GND", 275, 1224.92, 0, "input"),
         ],
         "caps": [
             ("cap", 282, 1230, ("VCC_3V3", 282, 1232.54, 0, "input"), ("GND", 282, 1227.46, 0, "input")),
             ("cap_0805", 289, 1230, ("VCC_3V3", 289, 1232.54, 0, "input"), ("GND", 289, 1227.46, 0, "input")),
         ]},
        # L1 for TPS54560 3.3V
        {"type": "symbol", "model": "inductor", "ref": "L1", "value": "4.7uH", "x": 275, "y": 1240},

        # U42: TPS54560 48V→5V/2A (fan supply)
        {"type": "regulator_block", "model": "tps54560", "ref": "U42", "value": "TPS54560 48V→5V/2A (Fan)",
         "x": 400, "y": 1230,
         "globals": [
             ("VCC_48V_FUSED", 385, 1237.62, 180, "input"),
             ("VCC_5V", 415, 1237.62, 0, "output"),
             ("GND", 415, 1224.92, 0, "input"),
         ],
         "caps": [
             ("cap", 422, 1230, ("VCC_5V", 422, 1232.54, 0, "input"), ("GND", 422, 1227.46, 0, "input")),
             ("cap_0805", 429, 1230, ("VCC_5V", 429, 1232.54, 0, "input"), ("GND", 429, 1227.46, 0, "input")),
         ]},
        # L2 for TPS54560 5V
        {"type": "symbol", "model": "inductor", "ref": "L2", "value": "4.7uH", "x": 415, "y": 1240},

        # U43: AP2112K 3.3V→1.8V (eMMC VCCQ)
        {"type": "regulator_block", "model": "ap2112k", "ref": "U43", "value": "AP2112K 3.3V→1.8V",
         "x": 540, "y": 1230,
         "globals": [
             ("VCC_3V3", 527, 1232.54, 180, "input"),
             ("GND", 527, 1227.46, 180, "input"),
             ("VCC_1V8", 553, 1232.54, 0, "output"),
         ],
         "caps": [
             ("cap", 560, 1230, ("VCC_1V8", 560, 1232.54, 0, "input"), ("GND", 560, 1227.46, 0, "input")),
         ]},

        # J2: Fan header 2-pin (5V)
        {"type": "symbol", "model": "fan_header", "ref": "J2", "value": "Fan 40mm 5V", "x": 480, "y": 1250},
        {"type": "global_labels", "labels": [
            ("VCC_5V", 490, 1252.54, 0, "input"),
            ("GND", 490, 1247.46, 0, "input"),
        ]},

        # Bulk caps near regulators
        {"type": "bulk_caps", "count": 4, "model": "cap_0805", "value": "10uF",
         "x_base": 220, "y": 1260, "x_step": 15, "vcc_label": "VCC_3V3"},
        {"type": "bulk_caps", "count": 2, "model": "cap_0805", "value": "10uF",
         "x_base": 400, "y": 1260, "x_step": 15, "vcc_label": "VCC_5V"},

        # Y1: 25MHz oscillator
        {"type": "symbol", "model": "osc_25mhz", "ref": "Y1", "value": "25MHz Oscillator", "x": 200, "y": 1280},
        {"type": "global_labels", "labels": [
            ("VCC_3V3", 190, 1282.54, 180, "input"),
            ("GND", 190, 1277.46, 180, "input"),
            ("CLK_25MHZ", 210, 1282.54, 0, "output"),
        ]},

        # ======================================================
        # Section 6: Status LEDs (edge)
        # D1-D8: PWR, SYS, ERR, NET, PIPE_ACTIVE, PIPE_DONE, HX-A, HX-B
        # ======================================================
        {"type": "text", "text": "Section 6: Status LEDs (8x)", "x": 15, "y": 1300, "size": 1.5},
        {"type": "node_led_pair", "led_ref": "D1", "led_value": "PWR_GREEN", "lx": 20, "ly": 1315,
         "r_ref": "R1", "r_value": "330", "rx": 10, "ry": 1315},
        {"type": "node_led_pair", "led_ref": "D2", "led_value": "SYS_BLUE", "lx": 20, "ly": 1327,
         "r_ref": "R2", "r_value": "330", "rx": 10, "ry": 1327},
        {"type": "node_led_pair", "led_ref": "D3", "led_value": "ERR_RED", "lx": 20, "ly": 1339,
         "r_ref": "R3", "r_value": "330", "rx": 10, "ry": 1339},
        {"type": "node_led_pair", "led_ref": "D4", "led_value": "NET_YELLOW", "lx": 20, "ly": 1351,
         "r_ref": "R4", "r_value": "330", "rx": 10, "ry": 1351},
        {"type": "node_led_pair", "led_ref": "D5", "led_value": "PIPE_ACTIVE_GREEN", "lx": 20, "ly": 1363,
         "r_ref": "R5", "r_value": "330", "rx": 10, "ry": 1363},
        {"type": "node_led_pair", "led_ref": "D6", "led_value": "PIPE_DONE_GREEN", "lx": 20, "ly": 1375,
         "r_ref": "R6", "r_value": "330", "rx": 10, "ry": 1375},
        {"type": "node_led_pair", "led_ref": "D7", "led_value": "HX-A_GREEN", "lx": 20, "ly": 1387,
         "r_ref": "R7", "r_value": "330", "rx": 10, "ry": 1387},
        {"type": "node_led_pair", "led_ref": "D8", "led_value": "HX-B_GREEN", "lx": 20, "ly": 1399,
         "r_ref": "R8", "r_value": "330", "rx": 10, "ry": 1399},

        # Fill remaining decoupling caps
        {"type": "fill_caps", "model": "cap", "value": "100nF", "limit": 120, "cols": 16,
         "x_base": 60, "y_base": 1420, "x_step": 8, "y_step": 6,
         "start_offset": lambda idx: ((idx - 1) % 16, (idx - 1) // 16)},
        {"type": "fill_caps", "model": "cap_0805", "value": "10uF", "limit": 180, "cols": 14,
         "x_base": 250, "y_base": 1420, "x_step": 10, "y_step": 6,
         "start_offset": lambda idx: ((idx - 121) % 14, (idx - 121) // 14)},
    ],
}

# ============================================================
# PCB DEFINITION -- 200x150mm 4-layer
# ============================================================

_pcb_footprints = []

# Top-left: 4x RP2354B + eMMC + flash
# U1-U4: RP2354B nodes
for i in range(4):
    _pcb_footprints.append((MODELS["rp2354b"]["footprint"], f"U{1+i}", f"RP2354B N{i}", 20 + i * 22, 18))
# U5-U8: W25Q128 flash for RP nodes
for i in range(4):
    _pcb_footprints.append((MODELS["w25q128"]["footprint"], f"U{5+i}", f"W25Q128 N{i}", 20 + i * 22, 8))
# U37-U38: eMMC near Node0
_pcb_footprints.append((MODELS["emmc"]["footprint"], "U37", "eMMC #0", 20, 32))
_pcb_footprints.append((MODELS["emmc"]["footprint"], "U38", "eMMC #1", 40, 32))

# Center: 12x LIFCL-17 in 2 rows of 6 + R-2R resistor networks
for eng in range(12):
    row = eng // 6
    col = eng % 6
    x = 30 + col * 25
    y = 55 + row * 30
    _pcb_footprints.append((MODELS["lifcl17"]["footprint"], f"U{9+eng}", f"LIFCL-17 DSP{eng}", x, y))

# U23-U34: W25Q32 config flash for LIFCL-17 engines (nearby)
for eng in range(12):
    row = eng // 6
    col = eng % 6
    x = 30 + col * 25
    y = 48 + row * 30
    _pcb_footprints.append((MODELS["w25q32"]["footprint"], f"U{23+eng}", f"W25Q32 DSP{eng}", x, y))

# R-2R ladder resistor networks between LIFCL-17 stages (footprint placeholders)
for ladder in range(11):
    row = ladder // 6
    col = ladder % 6
    rx = 42 + col * 25
    ry = 62 + row * 30
    # Each ladder is 16 discrete 0402 resistors
    for rr in range(16):
        r_num = 9 + ladder * 16 + rr
        _pcb_footprints.append((MODELS["resistor"]["footprint"], f"R{r_num}",
                                "10k" if rr % 2 == 0 else "20k", rx - 3 + (rr % 4) * 2, ry - 2 + (rr // 4) * 1.5))

# Left edge: 2x iCE40HX4K
_pcb_footprints.append((MODELS["ice40hx4k"]["footprint"], "U21", "iCE40HX4K-A", 12, 65))
_pcb_footprints.append((MODELS["ice40hx4k"]["footprint"], "U22", "iCE40HX4K-B", 12, 95))
# U35-U36: HX config flash
_pcb_footprints.append((MODELS["w25q32"]["footprint"], "U35", "W25Q32 HX-A", 12, 55))
_pcb_footprints.append((MODELS["w25q32"]["footprint"], "U36", "W25Q32 HX-B", 12, 85))

# Top-right: KSZ8081 + RJ45 + TPS2379
_pcb_footprints.append((MODELS["ksz8081"]["footprint"], "U39", "KSZ8081RNA", 170, 15))
_pcb_footprints.append((MODELS["power_conn"]["footprint"], "J1", "RJ45 MagJack", 188, 12))
_pcb_footprints.append((MODELS["tps2379"]["footprint"], "U40", "TPS2379", 180, 25))

# Bottom: Power supply, fan header
_pcb_footprints.append((MODELS["tps54560"]["footprint"], "U41", "TPS54560 3.3V", 60, 138))
_pcb_footprints.append((MODELS["tps54560"]["footprint"], "U42", "TPS54560 5V Fan", 100, 138))
_pcb_footprints.append((MODELS["ap2112k"]["footprint"], "U43", "AP2112K 1.8V", 140, 138))
_pcb_footprints.append((MODELS["inductor"]["footprint"], "L1", "4.7uH", 75, 144))
_pcb_footprints.append((MODELS["inductor"]["footprint"], "L2", "4.7uH", 115, 144))
_pcb_footprints.append((MODELS["polyfuse"]["footprint"], "F1", "1A", 40, 144))
_pcb_footprints.append((MODELS["fan_header"]["footprint"], "J2", "Fan 40mm", 160, 144))

# Y1: 25MHz oscillator
_pcb_footprints.append((MODELS["osc_25mhz"]["footprint"], "Y1", "25MHz", 12, 110))

# Right edge: LEDs
for i in range(8):
    _pcb_footprints.append((MODELS["led"]["footprint"], f"D{i+1}", f"LED{i+1}", 195, 40 + i * 5))
    _pcb_footprints.append((MODELS["resistor"]["footprint"], f"R{i+1}", "330", 191, 40 + i * 5))

# IC positions for decoupling cap placement
_ic_positions = (
    [(20 + i * 22, 18) for i in range(4)]          # RP2354B nodes
    + [(20, 32), (40, 32)]                           # eMMC
    + [(30 + (e % 6) * 25, 55 + (e // 6) * 30) for e in range(12)]  # LIFCL-17
    + [(12, 65), (12, 95)]                           # HX4K-A, HX4K-B
    + [(170, 15)]                                    # KSZ8081
    + [(60, 138), (100, 138), (140, 138)]            # Regulators
)

NODE_PCB_DEF = {
    "width": 200, "height": 150, "layers": 4,
    "title_text": "Hydra Mesh v11.0 -- INT8 Analogue Dataflow Engine -- 200x150mm 4-layer",
    "title_x": 100, "title_y": 4,
    "mounting_holes": [(4, 4), (196, 4), (4, 146), (196, 146)],
    "footprints": _pcb_footprints,
    "cap_steps": [
        {"fn": _pcb_ic_caps, "ic_positions": _ic_positions},
        {"fn": _pcb_fill_100nf, "limit": 100},
        {"fn": _pcb_bulk_caps, "limit": 160},
    ],
}

# ============================================================
# GENERATION FUNCTIONS (thin wrappers around engine)
# ============================================================

def gen_dataflow_sch(output_dir):
    run_sch_engine(NODE_BOARD, output_dir, "hydra_dataflow.kicad_sch")

def gen_dataflow_pcb(output_dir):
    run_pcb_engine(NODE_PCB_DEF, output_dir, "hydra_dataflow.kicad_pcb")

# ============================================================
# MAIN
# ============================================================

def main():
    print("Hydra Mesh v11.0 -- INT8 Analogue Dataflow Engine")
    print("Generating KiCad 8 files...\n")

    out_dir = os.path.join(OUTPUT_DIR, "hydra_dataflow")
    os.makedirs(out_dir, exist_ok=True)
    reset_uuids()
    gen_dataflow_sch(out_dir)
    gen_dataflow_pcb(out_dir)

    print("\nDone! Generated files:")
    for root, dirs, files in os.walk(out_dir):
        for f in sorted(files):
            fp = os.path.join(root, f)
            print(f"  {fp}  ({os.path.getsize(fp)} bytes)")

if __name__ == "__main__":
    main()
