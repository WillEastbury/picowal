// weight_buffer.sv — Ping-pong double-buffered weight tile storage
// Two BRAM banks: one fills from DMA while the other feeds the systolic array

module weight_buffer
  import hydra_infer_pkg::*;
#(
  parameter int N = ARRAY_N
)(
  input  logic    clk,
  input  logic    rst_n,

  // DMA write port — fills the inactive bank
  input  logic                     wr_en,
  input  logic [$clog2(N*N)-1:0]  wr_addr,
  input  int8_t                    wr_data,
  output logic                     wr_ready,    // can accept writes

  // Array read port — reads the active bank as NxN tile
  input  logic                     rd_req,      // request full tile
  output int8_t                    tile_out [N][N],
  output logic                     tile_valid,

  // Bank control
  input  logic                     swap_banks,  // swap active/fill banks
  output logic                     fill_done    // fill bank is fully written
);

  // ── Bank storage ───────────────────────────────────────────
  int8_t bank0 [N*N];
  int8_t bank1 [N*N];

  logic active_bank;  // 0 = bank0 is read, bank1 is write; 1 = opposite
  logic [$clog2(N*N):0] fill_count;

  // ── Bank swap ──────────────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      active_bank <= 1'b0;
      fill_count  <= '0;
    end else if (swap_banks) begin
      active_bank <= ~active_bank;
      fill_count  <= '0;
    end else if (wr_en && wr_ready) begin
      fill_count <= fill_count + 1;
    end
  end

  assign fill_done = (fill_count == N * N);
  assign wr_ready  = !fill_done;

  // ── Write port ─────────────────────────────────────────────
  always_ff @(posedge clk) begin
    if (wr_en && wr_ready) begin
      if (active_bank == 1'b0)
        bank1[wr_addr] <= wr_data;  // fill bank1 while reading bank0
      else
        bank0[wr_addr] <= wr_data;  // fill bank0 while reading bank1
    end
  end

  // ── Read port — combinational tile readout ─────────────────
  always_comb begin
    for (int c = 0; c < N; c++) begin
      for (int r = 0; r < N; r++) begin
        if (active_bank == 1'b0)
          tile_out[c][r] = bank0[c * N + r];
        else
          tile_out[c][r] = bank1[c * N + r];
      end
    end
  end

  // ── Tile valid ─────────────────────────────────────────────
  logic tile_valid_r;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      tile_valid_r <= 1'b0;
    else
      tile_valid_r <= rd_req;
  end

  assign tile_valid = tile_valid_r;

endmodule
