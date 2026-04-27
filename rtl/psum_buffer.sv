// psum_buffer.sv — Partial sum scratchpad for K-dimension tiling
// When K > ARRAY_N, partial sums must be stored and reloaded between tiles
// Banked BRAM: one column of partial sums per bank

module psum_buffer
  import hydra_infer_pkg::*;
#(
  parameter int N     = ARRAY_N,
  parameter int DEPTH = 256       // max M-tiles that can be buffered
)(
  input  logic    clk,
  input  logic    rst_n,

  // Write port — store partial sums from systolic array
  input  logic                       wr_en,
  input  logic [$clog2(DEPTH)-1:0]  wr_addr,    // M-tile index
  input  int32_t                     wr_data [N], // one column of psums

  // Read port — reload partial sums for accumulation
  input  logic                       rd_en,
  input  logic [$clog2(DEPTH)-1:0]  rd_addr,
  output int32_t                     rd_data [N],
  output logic                       rd_valid,

  // Clear a row (start of new output tile)
  input  logic                       clear_en,
  input  logic [$clog2(DEPTH)-1:0]  clear_addr
);

  // ── Storage: N banks × DEPTH entries ───────────────────────
  int32_t mem [N][DEPTH];

  // ── Write ──────────────────────────────────────────────────
  always_ff @(posedge clk) begin
    if (clear_en) begin
      for (int i = 0; i < N; i++)
        mem[i][clear_addr] <= '0;
    end else if (wr_en) begin
      for (int i = 0; i < N; i++)
        mem[i][wr_addr] <= wr_data[i];
    end
  end

  // ── Read (1-cycle latency) ─────────────────────────────────
  int32_t rd_data_r [N];
  logic   rd_valid_r;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rd_valid_r <= 1'b0;
      for (int i = 0; i < N; i++)
        rd_data_r[i] <= '0;
    end else begin
      rd_valid_r <= rd_en;
      if (rd_en) begin
        for (int i = 0; i < N; i++)
          rd_data_r[i] <= mem[i][rd_addr];
      end
    end
  end

  assign rd_data  = rd_data_r;
  assign rd_valid = rd_valid_r;

endmodule
