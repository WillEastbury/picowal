// ice40_stub.v -- Stub for iCE40 primitives (simulation only)
module SB_PLL40_CORE #(
    parameter FEEDBACK_PATH = "SIMPLE",
    parameter [3:0] DIVR = 4'd0,
    parameter [6:0] DIVF = 7'd0,
    parameter [2:0] DIVQ = 3'd0,
    parameter [2:0] FILTER_RANGE = 3'd0
)(
    input  REFERENCECLK,
    output PLLOUTCORE,
    output LOCK,
    input  RESETB,
    input  BYPASS
);
    assign PLLOUTCORE = REFERENCECLK;
    // Delay lock by a few cycles so reset logic triggers
    reg lock_reg;
    initial lock_reg = 1'b0;
    always @(posedge REFERENCECLK)
        lock_reg <= 1'b1;
    assign LOCK = lock_reg;
endmodule
