// bitnet_hx8k_splice.v — Pack multiple BitNet tiles into one HX8K, splice across chips
//
// HX8K: 7,680 LUT4, 208 GPIO (CT256 BGA)
// BitNet 8×8 tile: ~1,150 LUTs
// Tiles per chip: 6 (with ~800 LUTs for routing/control/IO regs)
//
// INTERNAL: 6 tiles pipelined inside one chip, no pin cost
// EXTERNAL: only chip-to-chip boundary needs pins
//
// Pin budget per chip:
//   Input:   8 × 8-bit = 64 pins  (from previous chip or host)
//   Output:  8 × 8-bit = 64 pins  (to next chip — requantized to 8-bit)
//   Control: clk + rst_n + valid_in + valid_out = 4 pins
//   Config:  SPI for bitstream load = 4 pins
//   Total:   136 pins of 208 available → comfortable
//
// What 6 tiles buys you:
//   - 6 layers of an 8×8 network inside ONE chip
//   - Single-cycle pipeline per layer, 6-clock latency per chip
//   - At 133MHz: 133M inferences/sec, 6 layers deep
//
// Splicing N chips:
//   CHIP 0 (layers 0-5) → CHIP 1 (layers 6-11) → CHIP 2 (layers 12-17) → ...
//   8-bit requantized bus between chips (64 wires + 4 control)
//
//   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
//   │   HX8K #0   │    │   HX8K #1   │    │   HX8K #2   │
//   │ ┌─┬─┬─┬─┬─┬─┐│    │ ┌─┬─┬─┬─┬─┬─┐│    │ ┌─┬─┬─┬─┬─┬─┐│
//   │ │0│1│2│3│4│5││──▶│ │6│7│8│9│A│B││──▶│ │C│D│E│F│G│H││──▶
//   │ └─┴─┴─┴─┴─┴─┘│    │ └─┴─┴─┴─┴─┴─┘│    │ └─┴─┴─┴─┴─┴─┘│
//   └─────────────┘    └─────────────┘    └─────────────┘
//      64 wires           64 wires           64 wires
//
// For 1000 tiles: 1000/6 = 167 chips × £4 = £668
// Pipeline: 1000 clocks latency, 133M inf/sec throughput
// Power: 167 × ~200mW = ~33W
//
// For wide matrices (>8):
//   Fan-out: duplicate input to N parallel chips
//   Accumulate: sum partial results (extra chip or host CPU)
//
//   ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐
//   │HX8K │ │HX8K │ │HX8K │ │HX8K │  4 chips = 32-wide input
//   │x0:7 │ │x8:15│ │x16:23│ │x24:31│
//   └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘
//      └───────┴───┬───┴───────┘
//                  ▼
//           accumulator chip
//           (or host RP2354B)

module bitnet_hx8k_splice #(
    parameter TILES_PER_CHIP = 6,
    parameter VEC_W = 8
)(
    input  wire        clk,
    input  wire        rst_n,

    // Chip-to-chip input (from previous chip or host)
    input  wire        chip_valid_in,
    input  wire [7:0]  chip_x_in [0:VEC_W-1],

    // Chip-to-chip output (to next chip or host)
    output wire        chip_valid_out,
    output wire [7:0]  chip_y_out [0:VEC_W-1]   // requantized 8-bit
);

    // =====================================================================
    // Internal pipeline: TILES_PER_CHIP layers, all inside this chip
    // No pin cost for internal connections — just fabric routing
    // =====================================================================

    wire [15:0] stage_data [0:TILES_PER_CHIP][0:VEC_W-1];
    wire        stage_valid [0:TILES_PER_CHIP];

    // Input stage: 8-bit → 16-bit sign extension
    genvar v;
    generate
        for (v = 0; v < VEC_W; v = v + 1) begin : in_ext
            assign stage_data[0][v] = {{8{chip_x_in[v][7]}}, chip_x_in[v]};
        end
    endgenerate
    assign stage_valid[0] = chip_valid_in;

    // Instantiate tile pipeline
    genvar t;
    generate
        for (t = 0; t < TILES_PER_CHIP; t = t + 1) begin : tile

            // Requantize 16→8 between internal stages
            wire [7:0] rq [0:VEC_W-1];
            genvar rv;
            for (rv = 0; rv < VEC_W; rv = rv + 1) begin : requant
                wire signed [15:0] sv = stage_data[t][rv];
                assign rq[rv] = (sv > 16'sd127)  ? 8'd127  :
                                (sv < -16'sd128) ? -8'd128 :
                                sv[7:0];
            end

            wire [15:0] tile_y [0:VEC_W-1];

            bitnet_tile_8x8 matmul (
                .clk       (clk),
                .rst_n     (rst_n),
                .valid_in  (stage_valid[t]),
                .x_in      (rq),
                .valid_out (stage_valid[t+1]),
                .y_out     (tile_y),
                // ReLU on all but last tile in chip
                .relu_en   (t < TILES_PER_CHIP - 1)
            );

            for (rv = 0; rv < VEC_W; rv = rv + 1) begin : fwd
                assign stage_data[t+1][rv] = tile_y[rv];
            end
        end
    endgenerate

    // =====================================================================
    // Output stage: requantize 16→8 for chip-to-chip bus
    // This is the ONLY place we cross chip boundaries
    // 8 × 8-bit = 64 pins — well within budget
    // =====================================================================

    reg        out_valid_r;
    reg [7:0]  out_data_r [0:VEC_W-1];

    integer j;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_valid_r <= 0;
            for (j = 0; j < VEC_W; j = j + 1)
                out_data_r[j] <= 8'd0;
        end else begin
            out_valid_r <= stage_valid[TILES_PER_CHIP];
            for (j = 0; j < VEC_W; j = j + 1) begin
                // Saturating requantize
                if ($signed(stage_data[TILES_PER_CHIP][j]) > 16'sd127)
                    out_data_r[j] <= 8'd127;
                else if ($signed(stage_data[TILES_PER_CHIP][j]) < -16'sd128)
                    out_data_r[j] <= -8'd128;
                else
                    out_data_r[j] <= stage_data[TILES_PER_CHIP][j][7:0];
            end
        end
    end

    assign chip_valid_out = out_valid_r;
    generate
        for (v = 0; v < VEC_W; v = v + 1) begin : out_assign
            assign chip_y_out[v] = out_data_r[v];
        end
    endgenerate

endmodule
