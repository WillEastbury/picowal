// switched_matmul.v — Full 8×8 INT8 matrix multiply via switching
// C[8×8] = W[8×8] × X[8×1], computed 4 rows per cycle (HX8K) or 2 (HX4K)
//
// Architecture:
//   - Weights precomputed into BRAM lookup tables at model load time
//   - Input vector X broadcast to all dot-product units simultaneously
//   - Each dot-product unit: 8 BRAM lookups + adder tree
//   - ZERO multiply gates. All products are SWITCHED, not calculated.
//
// HX8K config (N_ROWS=4): 32 BRAM + ~480 LUTs. 8×8 matmul in 2 cycles.
// HX4K config (N_ROWS=2): 16 BRAM + ~240 LUTs. 8×8 matmul in 4 cycles.
//
// Pipeline: 4 cycles latency (1 BRAM + 3 adder tree), then 1 result/cycle
//
// To compute larger matrices (M×K × K×1):
//   - K > 8: accumulate across multiple passes (external accumulator)
//   - M > N_ROWS: cycle through row groups (M / N_ROWS passes)

module switched_matmul #(
    parameter N_ROWS   = 4,  // parallel dot-product units (4 for HX8K)
    parameter DOT_LEN  = 8,  // dot product length (columns of W, length of X)
    parameter OUT_BITS = 19  // output width per element
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Inference interface ---
    input  wire        compute,          // pulse: start one matmul pass
    input  wire [7:0]  x [0:DOT_LEN-1], // input vector (broadcast to all rows)

    // --- Weight loading interface ---
    // To precompute: for each row r, position k, input val v:
    //   table[r][k][v] = weight[r][k] * (signed)v
    input  wire [$clog2(N_ROWS)-1:0] wl_row,   // which row's dot unit
    input  wire [2:0]                 wl_pos,   // which position (0-7) within row
    input  wire [7:0]                 wl_addr,  // input value (0-255)
    input  wire [15:0]                wl_data,  // precomputed product
    input  wire                       wl_we,    // write enable

    // --- Results ---
    output wire [OUT_BITS-1:0] result [0:N_ROWS-1],
    output wire [N_ROWS-1:0]   result_valid,

    // --- Row group select (for matrices bigger than N_ROWS) ---
    // External controller increments this to walk through row groups
    input  wire [$clog2(N_ROWS)-1:0] row_group  // currently unused, for expansion
);

    // =====================================================================
    // Instantiate N_ROWS parallel dot-product units
    // Each unit: 8 BRAM (switched multiply) + 1 adder tree
    // Total BRAM: N_ROWS × 8
    // =====================================================================

    genvar r;
    generate
        for (r = 0; r < N_ROWS; r = r + 1) begin : dot_row

            // Weight load routing: only this row's unit gets writes
            wire       row_we  = wl_we && (wl_row == r);

            switched_dot8 dot (
                .clk       (clk),
                .rst_n     (rst_n),
                .in_valid  (compute),

                // Broadcast input vector to all rows
                .x0 (x[0]), .x1 (x[1]), .x2 (x[2]), .x3 (x[3]),
                .x4 (x[4]), .x5 (x[5]), .x6 (x[6]), .x7 (x[7]),

                // Weight loading
                .w_sel   (wl_pos),
                .w_addr  (wl_addr),
                .w_data  (wl_data),
                .w_we    (row_we),

                // Output
                .dot_out   (result[r]),
                .out_valid (result_valid[r])
            );

        end
    endgenerate

endmodule
