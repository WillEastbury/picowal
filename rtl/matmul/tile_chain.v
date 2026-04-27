// tile_chain.v — Chain of FPGA matmul tiles with SRAM overflow buffer
//
// CHAIN THEM:
//
//   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
//   │ TILE 0   │──▶│ TILE 1   │──▶│ TILE 2   │──▶│ TILE 3   │──▶ out
//   │ 8×8 L0   │   │ 8×8 L1   │   │ 8×8 L2   │   │ 8×8 L3   │
//   │ HX8K     │   │ HX8K     │   │ HX8K     │   │ HX8K     │
//   │ 136 GOPS │   │ 136 GOPS │   │ 136 GOPS │   │ 136 GOPS │
//   └──────────┘   └──────────┘   └──────────┘   └──────────┘
//
//   Each tile: weights baked into LUT bitstream
//   Inter-tile: 8 × 16-bit bus = 128 wires on PCB
//   Latency: 1 clock per tile = 4 clocks total
//   Throughput: 1 inference per clock (pipelined)
//   At 133MHz: 133M inferences/sec through 4-layer network
//
// WHEN PINS RUN OUT (wider than 8):
//
//   For 16×16 matmul: tile as 4 × 8×8 blocks
//   Each block fits in one HX8K, but need to ACCUMULATE across blocks
//   Partial results go to SRAM buffer, then load + add in next pass
//
//   ┌────────┐ ┌────────┐
//   │TILE 0A │ │TILE 0B │   Two tiles compute partial dot products
//   │A[0:7]  │ │A[8:15] │   for columns 0-7 and 8-15
//   └───┬────┘ └───┬────┘
//       │          │
//       ▼          ▼
//   ┌──────────────────┐
//   │  SRAM buffer     │   Store partial results
//   │  (accumulate)    │   Load both, add, store final
//   └────────┬─────────┘
//             │
//             ▼
//   ┌────────────────┐
//   │  TILE 1 (next  │   Next layer reads from SRAM
//   │  layer)        │
//   └────────────────┘

module tile_chain #(
    parameter N_TILES = 4,
    parameter VEC_W   = 8,     // vector width per tile
    parameter DAT_W   = 16     // data width between tiles
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- Chain input ----
    input  wire        in_valid,
    input  wire [7:0]  x_in [0:VEC_W-1],

    // ---- Chain output ----
    output wire        out_valid,
    output wire [DAT_W-1:0] y_out [0:VEC_W-1],

    // ---- SRAM overflow interface (active for wide matrix tiling) ----
    output reg  [17:0] sram_addr,
    output reg  [15:0] sram_wdata,
    input  wire [15:0] sram_rdata,
    output reg         sram_we,
    output reg         sram_re,

    // ---- Status ----
    output wire [N_TILES-1:0] tile_busy
);

    // =====================================================================
    // Inter-tile buses
    // =====================================================================

    wire [DAT_W-1:0] inter [0:N_TILES][0:VEC_W-1];
    wire [N_TILES:0] inter_valid;

    // Input to first tile: zero-extend 8-bit to 16-bit
    genvar v;
    generate
        for (v = 0; v < VEC_W; v = v + 1) begin : in_ext
            assign inter[0][v] = {{(DAT_W-8){x_in[v][7]}}, x_in[v]};
        end
    endgenerate
    assign inter_valid[0] = in_valid;

    // =====================================================================
    // Instantiate tile chain
    // Each tile takes 16-bit input, truncates to 8-bit for multiply,
    // outputs 16-bit result to next tile.
    // =====================================================================

    genvar t;
    generate
        for (t = 0; t < N_TILES; t = t + 1) begin : tile

            wire [7:0] tile_x [0:VEC_W-1];
            wire [15:0] tile_y [0:VEC_W-1];

            // Requantize: 16-bit → 8-bit input for this tile
            // Simple truncation with saturation
            genvar tv;
            for (tv = 0; tv < VEC_W; tv = tv + 1) begin : requant
                wire signed [15:0] sv = inter[t][tv];
                assign tile_x[tv] = (sv > 127)  ? 8'd127  :
                                    (sv < -128) ? -8'd128 :
                                                  sv[7:0];
            end

            tile_matmul_8x8 matmul (
                .clk       (clk),
                .rst_n     (rst_n),
                .valid_in  (inter_valid[t]),
                .x_in      (tile_x),
                .valid_out (inter_valid[t+1]),
                .y_out     (tile_y),
                .relu_en   (t < N_TILES - 1)    // ReLU on all but last layer
            );

            for (tv = 0; tv < VEC_W; tv = tv + 1) begin : fwd
                assign inter[t+1][tv] = tile_y[tv];
            end

            assign tile_busy[t] = inter_valid[t];
        end
    endgenerate

    // Output
    assign out_valid = inter_valid[N_TILES];
    generate
        for (v = 0; v < VEC_W; v = v + 1) begin : out_assign
            assign y_out[v] = inter[N_TILES][v];
        end
    endgenerate

    // =====================================================================
    // SRAM overflow controller
    // For matrices wider than VEC_W: accumulate partial results
    //
    // Usage: host configures slicing schedule via separate control bus.
    // This module provides the SRAM interface; the RP2354B orchestrates
    // the tiling sequence (which tiles to activate, when to accumulate).
    // =====================================================================

    // Simple passthrough for now — RP2354B manages SRAM directly
    // via PIO when doing wide-matrix tiling
    always @(posedge clk) begin
        sram_we   <= 0;
        sram_re   <= 0;
        sram_addr <= 0;
        sram_wdata <= 0;
    end

endmodule
