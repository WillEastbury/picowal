`timescale 1ns / 1ps
//============================================================================
// reset_sync.v — Two-FF asynchronous reset synchroniser for iCE40 HX8K
//
// Produces a synchronised active-low reset (rst_n) that stays asserted
// until BOTH conditions are met:
//   1. The PLL has locked (pll_locked == 1)
//   2. The external reset button is released (btn_rst_n == 1)
//
// Uses an async-assert / sync-deassert pattern to avoid metastability on
// the deassertion edge while still providing immediate reset assertion.
//============================================================================

module reset_sync (
    input  wire clk,          // system clock (from PLL output)
    input  wire pll_locked,   // PLL lock indicator
    input  wire btn_rst_n,    // external reset button (active low)
    output wire rst_n         // synchronised active-low reset
);

    // Combined raw reset: asserted when PLL unlocked OR button pressed
    wire rst_n_raw = pll_locked & btn_rst_n;

    // Two-stage synchroniser (async assert, sync deassert)
    reg [1:0] sync_ff;

    always @(posedge clk or negedge rst_n_raw) begin
        if (!rst_n_raw) begin
            sync_ff <= 2'b00;           // async assert (immediate)
        end else begin
            sync_ff <= {sync_ff[0], 1'b1};  // sync deassert (two-FF chain)
        end
    end

    assign rst_n = sync_ff[1];

endmodule
