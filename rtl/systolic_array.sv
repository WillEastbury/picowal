// systolic_array.sv — 64-column × 4-row weight-stationary systolic slice
// Each ECP5 runs one slice; 4 slices form the full 1024-MAC array
// Uses dual-packed MAC units (2 MACs per DSP) → 128 DSPs for 256 MACs
// Activations flow left-to-right across columns, rows accumulate depth

module systolic_array
  import hydra_infer_pkg::*;
#(
  parameter int COLS = ARRAY_COLS,  // 64
  parameter int ROWS = ARRAY_ROWS   // 4
)(
  input  logic    clk,
  input  logic    rst_n,

  // Control
  input  logic    clear_acc,
  input  logic    load_weights,
  input  logic    valid_in,

  // Weight loading — [col][row]
  input  int8_t   weight_in [COLS][ROWS],

  // Activation input — one per row, fed from left
  input  int8_t   act_in    [ROWS],

  // Partial sum output — one per column, drained from bottom row
  output int32_t  psum_out  [COLS],
  output logic    psum_valid
);

  // ── PE weight registers ────────────────────────────────────
  int8_t pe_weight [ROWS][COLS];

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
          pe_weight[r][c] <= '0;
    end else if (load_weights) begin
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
          pe_weight[r][c] <= weight_in[c][r];
    end
  end

  // ── Activation propagation ─────────────────────────────────
  int8_t act_pipe [ROWS][COLS+1];

  always_comb begin
    for (int r = 0; r < ROWS; r++)
      act_pipe[r][0] = act_in[r];
  end

  // ── Valid propagation ──────────────────────────────────────
  logic valid_pipe [COLS+1];
  assign valid_pipe[0] = valid_in;

  // ── MAC units — dual-packed, one DSP per 2 adjacent rows ──
  // For ROWS=4: pair (row0,row1) and (row2,row3) per column
  int32_t pe_acc [ROWS][COLS];
  logic   pe_valid [ROWS][COLS];

  generate
    for (genvar c = 0; c < COLS; c++) begin : gen_col
      for (genvar rp = 0; rp < ROWS/2; rp++) begin : gen_row_pair
        localparam int r0 = rp * 2;
        localparam int r1 = rp * 2 + 1;

        mac_unit u_mac (
          .clk           (clk),
          .rst_n         (rst_n),
          .clear_acc     (clear_acc),
          .valid_in      (valid_pipe[c]),
          .weight_0      (pe_weight[r0][c]),
          .weight_1      (pe_weight[r1][c]),
          .activation_0  (act_pipe[r0][c]),
          .activation_1  (act_pipe[r1][c]),
          .accumulator_0 (pe_acc[r0][c]),
          .accumulator_1 (pe_acc[r1][c]),
          .valid_out     (pe_valid[r0][c])
        );

        assign pe_valid[r1][c] = pe_valid[r0][c];
      end
    end
  endgenerate

  // ── Activation pass-through pipeline ───────────────────────
  generate
    for (genvar r = 0; r < ROWS; r++) begin : gen_act_row
      for (genvar c = 0; c < COLS; c++) begin : gen_act_col
        always_ff @(posedge clk or negedge rst_n) begin
          if (!rst_n)
            act_pipe[r][c+1] <= '0;
          else
            act_pipe[r][c+1] <= act_pipe[r][c];
        end
      end
    end
  endgenerate

  // ── Valid pipeline ─────────────────────────────────────────
  generate
    for (genvar c = 0; c < COLS; c++) begin : gen_vpipe
      always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
          valid_pipe[c+1] <= 1'b0;
        else
          valid_pipe[c+1] <= valid_pipe[c];
      end
    end
  endgenerate

  // ── Column output: sum across rows for each column ─────────
  // Reduction tree: add all ROWS accumulators per column
  always_comb begin
    for (int c = 0; c < COLS; c++) begin
      psum_out[c] = '0;
      for (int r = 0; r < ROWS; r++)
        psum_out[c] = psum_out[c] + pe_acc[r][c];
    end
  end

  assign psum_valid = pe_valid[ROWS-1][COLS-1];

endmodule
