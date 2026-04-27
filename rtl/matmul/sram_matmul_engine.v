// sram_matmul_engine.v — Full matrix-vector multiply using SRAM switching
//
// Architecture: N_UNITS parallel dot-product units, each with 8 external SRAMs.
// All units share the same input vector (broadcast bus).
// Each unit computes a different output row simultaneously.
//
// Configuration examples:
//   N_UNITS=1:  8 SRAMs,  1 row/cycle,  ~$16 SRAM cost
//   N_UNITS=4: 32 SRAMs,  4 rows/cycle, ~$64 SRAM cost  ← sweet spot
//   N_UNITS=8: 64 SRAMs,  8 rows/cycle, ~$128 SRAM cost ← full 8×8 in 1 cycle
//
// For 1024×8 weight matrix with N_UNITS=4:
//   1024 rows ÷ 4 parallel = 256 cycles × 2 clocks = 512 clocks
//   At 133MHz: 3.8μs per matrix-vector multiply
//   = 1024 × 8 × 2 / 3.8μs = 4.3 GOPS
//
// For 1024×1024 weight matrix (chunked K=8 at a time, external accumulation):
//   (1024/8) × (1024/4) × 2 = 65,536 clocks = 492μs
//   = 1024 × 1024 × 2 / 492μs = 4.26 GOPS
//
// FPGA resource: N_UNITS × ~120 LUTs (adder trees) + ~200 LUTs (control)
// HX4K (3520 LUT): up to N_UNITS=27 (but 27×8 = 216 SRAM pins — GPIO limited)
// HX8K (7680 LUT): up to N_UNITS=62 (pin limited way before LUT limited)
//
// Real pin limit (HX8K-CT256 has 208 GPIO):
//   Per SRAM: 18 addr + 16 data + 3 control = 37 pins
//   But SRAMs in same unit SHARE addr[17:8] (weight_sel) and control
//   Per unit: 8 unique addr[7:0] + shared addr[17:8] + 8×16 data + 8×3 ctrl
//           = 8×8 + 10 + 128 + 24 = 226 pins for first unit (too many!)
//
// Solution: multiplex the data bus. Each unit time-shares 1 data bus across 8 SRAMs.
// OR: use multiple FPGAs (fan-out topology) — each HX handles 2 units.
//
// Practical config: N_UNITS=2 per HX8K, 4× HX8K for 8 parallel rows.

module sram_matmul_engine #(
    parameter N_UNITS   = 4,    // parallel dot-product units
    parameter DOT_LEN   = 8,    // elements per dot product
    parameter MAX_ROWS  = 1024  // max weight matrix rows
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- Host command interface ----
    input  wire        start_compute,
    input  wire [9:0]  num_rows,       // actual row count (up to MAX_ROWS)
    input  wire [7:0]  x_vec [0:DOT_LEN-1],  // input vector

    // ---- Weight loading ----
    input  wire        mode_write,
    input  wire [$clog2(N_UNITS)-1:0] wr_unit_sel,
    input  wire [2:0]  wr_sram_sel,
    input  wire [17:0] wr_addr,
    input  wire [15:0] wr_data,
    input  wire        wr_valid,

    // ---- Physical SRAM interfaces (active to chip pins) ----
    // N_UNITS × 8 SRAMs
    output wire [17:0] sram_addr [0:N_UNITS-1][0:DOT_LEN-1],
    inout  wire [15:0] sram_data [0:N_UNITS-1][0:DOT_LEN-1],
    output wire [N_UNITS*8-1:0] sram_ce_n,
    output wire [N_UNITS*8-1:0] sram_oe_n,
    output wire [N_UNITS*8-1:0] sram_we_n,

    // ---- Result stream ----
    output reg  [18:0] result_data,
    output reg         result_valid,
    output reg         done
);

    // Row sequencer: walks through rows N_UNITS at a time
    reg [9:0]  row_cursor;
    reg        computing;
    reg        compute_pulse;

    // Dot unit outputs
    wire [18:0] unit_result [0:N_UNITS-1];
    wire [N_UNITS-1:0] unit_valid;

    // Result collection
    reg [$clog2(N_UNITS)-1:0] out_idx;
    reg collecting;

    // ---- Instantiate N_UNITS dot-product units ----
    genvar u;
    generate
        for (u = 0; u < N_UNITS; u = u + 1) begin : unit

            wire [9:0] row_addr = row_cursor + u;

            wire this_wr_valid = wr_valid && (wr_unit_sel == u);

            sram_dot8 dot (
                .clk         (clk),
                .rst_n       (rst_n),
                .compute     (compute_pulse),
                .weight_sel  (row_addr),
                .x           (x_vec),
                .mode_write  (mode_write),
                .wr_sram_sel (wr_sram_sel),
                .wr_addr     (wr_addr),
                .wr_data     (wr_data),
                .wr_valid    (this_wr_valid),
                .sram_addr   (sram_addr[u]),
                .sram_data   (sram_data[u]),
                .sram_ce_n   (sram_ce_n[u*8 +: 8]),
                .sram_oe_n   (sram_oe_n[u*8 +: 8]),
                .sram_we_n   (sram_we_n[u*8 +: 8]),
                .dot_out     (unit_result[u]),
                .dot_valid   (unit_valid[u])
            );
        end
    endgenerate

    // ---- Row sequencer ----
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            computing     <= 0;
            compute_pulse <= 0;
            row_cursor    <= 0;
            result_valid  <= 0;
            collecting    <= 0;
            done          <= 0;
            out_idx       <= 0;
        end else begin
            compute_pulse <= 0;
            result_valid  <= 0;
            done          <= 0;

            if (start_compute && !computing) begin
                computing     <= 1;
                row_cursor    <= 0;
                compute_pulse <= 1;  // fire first batch
            end

            // Collect results when units are done
            if (computing && unit_valid[0]) begin
                collecting <= 1;
                out_idx    <= 0;
            end

            if (collecting) begin
                result_data  <= unit_result[out_idx];
                result_valid <= 1;

                if (out_idx == N_UNITS - 1) begin
                    collecting <= 0;
                    // Advance to next row group
                    if (row_cursor + N_UNITS >= num_rows) begin
                        computing <= 0;
                        done      <= 1;
                    end else begin
                        row_cursor    <= row_cursor + N_UNITS;
                        compute_pulse <= 1;
                    end
                end else begin
                    out_idx <= out_idx + 1;
                end
            end
        end
    end

endmodule
