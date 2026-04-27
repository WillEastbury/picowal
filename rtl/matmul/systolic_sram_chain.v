// systolic_sram_chain.v — Systolic MAC array made of SRAM chips
//
// THE KEY INSIGHT:
//   SRAM data output pins → next SRAM address input pins
//   Each SRAM precomputes the FULL MAC+requant operation as a lookup.
//   Partial sum propagates through the chain via physical wiring.
//   No adder tree. No FPGA arithmetic. The SRAM IS the MAC unit.
//
// How it works:
//
//   Each SRAM[k] contains a lookup table:
//     table[partial_8bit][x_8bit] = requant(partial + weight[k] × x)
//
//   Wiring:
//     SRAM[k].Data[15:8]  ──wire──▶  SRAM[k+1].Addr[15:8]  (partial sum)
//     CPU PIO              ──wire──▶  SRAM[k].Addr[7:0]      (input x[k])
//     CPU PIO              ──wire──▶  SRAM[k].Addr[17:16]    (row select)
//
//   Chain operation:
//     SRAM[0]: addr = {row, 0x00, x[0]}     → output = w[row][0] × x[0]
//     SRAM[1]: addr = {row, partial0, x[1]}  → output = partial0 + w[row][1] × x[1]
//     SRAM[2]: addr = {row, partial1, x[2]}  → output = partial1 + w[row][2] × x[2]
//     ...
//     SRAM[7]: addr = {row, partial6, x[7]}  → output = FINAL DOT PRODUCT
//
// Address map per SRAM (IS61WV25616BLL, 18-bit address):
//     A[17:16] = row select (4 rows per SRAM, time-multiplex for more)
//     A[15:8]  = partial sum from previous stage (8-bit, signed+offset → unsigned)
//     A[7:0]   = input activation x[k]
//
// Data output (16-bit):
//     D[15:8]  = new partial sum (8-bit requantized) → wired to next SRAM addr
//     D[7:0]   = full-precision low bits (for debug / extended precision)
//
// Resources per 8-element dot product:
//     8× IS61WV25616BLL SRAM ($16 total)
//     0 FPGA LUTs for compute (!!!)
//     1× 74HC574 or FPGA register per stage for clocked pipeline (optional)
//
// Latency:
//     Async cascade: 8 × 10ns = 80ns (but glitch-prone)
//     Clocked (registered per stage): 8 clocks × 10ns = 80ns, pipelined 1/clock
//
// Throughput (pipelined, 100MHz, 4 rows per SRAM):
//     1 dot product per clock = 100M dot/s
//     Each dot = 8 MACs × 2 ops = 16 ops → 1.6 GOPS per chain
//     4 parallel chains = 6.4 GOPS for $64 in SRAM
//
// CRITICAL: 8-bit partial sum limits dynamic range.
//     Max INT8 product: 127 × 127 = 16,129
//     8-bit partial sum range: 0-255 (unsigned) or ±128 (signed)
//     Must requantize aggressively at each stage.
//     Alternative: use 10-bit partial (A[17:16]=row, A[15:6]=partial, A[5:0]=input_6bit)
//     Trade-off: input precision vs accumulator precision.

module systolic_sram_chain #(
    parameter CHAIN_LEN = 8,    // number of SRAM stages (dot product width)
    parameter N_ROWS    = 4     // rows per SRAM (A[17:16] selects)
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- CPU interface (directly to SRAM address pins) ----
    input  wire [7:0]  x_in [0:CHAIN_LEN-1],  // input vector elements
    input  wire [1:0]  row_sel,                 // which of 4 rows (A[17:16])
    input  wire        start,                   // begin inference

    // ---- SRAM chip interfaces ----
    // Address buses: directly driven, active to SRAM pins
    // A[17:16] from row_sel (active to all chips)
    // A[15:8]  chain: chip 0 gets 0x00, chips 1-7 get previous chip's D[15:8]
    // A[7:0]   from x_in[k] (each chip gets its own input)
    output wire [17:0] sram_addr [0:CHAIN_LEN-1],

    // Data buses: directly from SRAM data pins
    // D[15:8] wired to next chip's A[15:8] on PCB
    // D[7:0] optional precision / debug
    input  wire [15:0] sram_data [0:CHAIN_LEN-1],

    // Control (active to all chips)
    output wire [CHAIN_LEN-1:0] sram_ce_n,
    output wire [CHAIN_LEN-1:0] sram_oe_n,

    // ---- Result ----
    output wire [7:0]  result,      // final dot product (from last SRAM D[15:8])
    output wire [7:0]  result_lo,   // low bits for extended precision
    output wire        result_valid
);

    // =====================================================================
    // Address wiring: THE CORE OF THE DESIGN
    //
    // This is where the magic happens. The partial sum propagates via
    // physical PCB traces from one SRAM's data pins to the next's address.
    // =====================================================================

    // Pipeline registers between stages (for clean clocked operation)
    reg [7:0] partial [0:CHAIN_LEN-1];  // registered partial sums
    reg [CHAIN_LEN-1:0] stage_valid;

    // First stage: partial sum = 0 (no previous accumulation)
    // Subsequent stages: partial sum = previous SRAM's D[15:8]
    //
    // NOTE: In the pure-hardware version, these registers would be
    // 74HC574 octal D flip-flops on the PCB, not FPGA resources.
    // The FPGA isn't in the data path at all.

    integer k;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (k = 0; k < CHAIN_LEN; k = k + 1)
                partial[k] <= 8'd128;  // bias offset (unsigned encoding of 0)
            stage_valid <= 0;
        end else begin
            // Stage 0: initial partial = 0 (encoded as 128 in unsigned)
            partial[0] <= 8'd128;
            stage_valid[0] <= start;

            // Stages 1+: partial comes from previous SRAM's data output
            for (k = 1; k < CHAIN_LEN; k = k + 1) begin
                partial[k] <= sram_data[k-1][15:8];  // D[15:8] → next A[15:8]
                stage_valid[k] <= stage_valid[k-1];
            end
        end
    end

    // =====================================================================
    // Address generation: combine row_sel + partial + input
    // =====================================================================

    genvar g;
    generate
        for (g = 0; g < CHAIN_LEN; g = g + 1) begin : addr_gen
            assign sram_addr[g] = {row_sel, partial[g], x_in[g]};
        end
    endgenerate

    // All SRAMs always active (active-low CE# and OE#)
    assign sram_ce_n = {CHAIN_LEN{1'b0}};
    assign sram_oe_n = {CHAIN_LEN{1'b0}};

    // =====================================================================
    // Result: last SRAM's data output
    // =====================================================================

    assign result    = sram_data[CHAIN_LEN-1][15:8];
    assign result_lo = sram_data[CHAIN_LEN-1][7:0];
    assign result_valid = stage_valid[CHAIN_LEN-1];

endmodule
