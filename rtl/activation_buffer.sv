// activation_buffer.sv — FIFO-based activation staging buffer
// Bridges DMA engine ↔ systolic array with backpressure

module activation_buffer
  import hydra_infer_pkg::*;
#(
  parameter int N     = ARRAY_N,
  parameter int DEPTH = ACT_BUF_DEPTH
)(
  input  logic    clk,
  input  logic    rst_n,

  // Write port (from DMA or previous layer output)
  input  logic    wr_en,
  input  int8_t   wr_data [N],     // N activations per write (one row vector)
  output logic    wr_ready,

  // Read port (to systolic array)
  input  logic    rd_en,
  output int8_t   rd_data [N],     // N activations per read
  output logic    rd_valid,

  // Status
  output logic [$clog2(DEPTH):0] fill_level
);

  // ── Storage ────────────────────────────────────────────────
  int8_t mem [DEPTH][N];
  logic [$clog2(DEPTH)-1:0] wr_ptr, rd_ptr;
  logic [$clog2(DEPTH):0]   count;

  assign fill_level = count;
  assign wr_ready   = (count < DEPTH);
  assign rd_valid   = (count > 0);

  // ── Write logic ────────────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      wr_ptr <= '0;
    end else if (wr_en && wr_ready) begin
      mem[wr_ptr] <= wr_data;
      wr_ptr      <= (wr_ptr == DEPTH - 1) ? '0 : wr_ptr + 1;
    end
  end

  // ── Read logic ─────────────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rd_ptr <= '0;
    end else if (rd_en && rd_valid) begin
      rd_ptr <= (rd_ptr == DEPTH - 1) ? '0 : rd_ptr + 1;
    end
  end

  // Combinational read
  assign rd_data = mem[rd_ptr];

  // ── Count tracking ─────────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= '0;
    end else begin
      case ({wr_en && wr_ready, rd_en && rd_valid})
        2'b10:   count <= count + 1;
        2'b01:   count <= count - 1;
        default: count <= count;
      endcase
    end
  end

endmodule
