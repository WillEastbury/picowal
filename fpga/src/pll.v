`timescale 1ns / 1ps
//============================================================================
// pll.v — PLL wrapper for iCE40 HX8K (Alchitry Cu board)
//
// Uses the SB_PLL40_CORE primitive to derive a 50 MHz system clock from
// the 100 MHz on-board oscillator (pin P7).
//
// PLL configuration (from iCE40 sysCLOCK PLL Design & Usage Guide):
//   F_REF     = 100 MHz / (DIVR + 1) = 100 / 1 = 100 MHz
//   F_VCO     = F_REF * (DIVF + 1)   = 100 * 8 = 800 MHz
//   F_PLLOUT  = F_VCO / 2^DIVQ       = 800 / 16 = 50 MHz
//
//   DIVR         = 4'b0000  (0  → reference divider = 1)
//   DIVF         = 7'b0000111 (7 → feedback divider  = 8)
//   DIVQ         = 3'b100  (4  → output divider      = 16)
//   FILTER_RANGE = 3'b001  (for F_PFD 70–130 MHz range)
//============================================================================

module pll (
    input  wire clk_100m,   // 100 MHz oscillator input (pin P7)
    output wire clk_50m,    // 50 MHz system clock output
    output wire pll_locked  // high when PLL has achieved lock
);

    SB_PLL40_CORE #(
        .FEEDBACK_PATH ("SIMPLE"),
        .PLLOUT_SELECT ("GENCLK"),
        .DIVR          (4'b0000),       // DIVR = 0
        .DIVF          (7'b0000111),    // DIVF = 7
        .DIVQ          (3'b100),        // DIVQ = 4
        .FILTER_RANGE  (3'b001)         // PFD range for ~100 MHz ref
    ) u_pll (
        .REFERENCECLK  (clk_100m),      // 100 MHz in
        .PLLOUTGLOBAL  (clk_50m),       // 50 MHz out (routed to global net)
        .PLLOUTCORE    (),              // unused core output
        .LOCK          (pll_locked),
        .BYPASS        (1'b0),          // normal PLL operation
        .RESETB        (1'b1),          // PLL always enabled (active-low reset)
        .LATCHINPUTVALUE (1'b0),        // not used
        .SDI           (1'b0),          // dynamic reconfig — unused
        .SDO           (),
        .SCLK          (1'b0)
    );

endmodule
