// mac_unit.sv — Dual INT8 multiply-accumulate packed into one ECP5 MULT18X18D
// Each DSP block computes two independent INT8 MACs per cycle
// Pack: {0, a1[7:0], 0, a0[7:0]} × {0, b1[7:0], 0, b0[7:0]} with guard bits

module mac_unit
  import hydra_infer_pkg::*;
(
  input  logic    clk,
  input  logic    rst_n,

  // Control
  input  logic    clear_acc,     // clear accumulators (new tile)
  input  logic    valid_in,      // input data valid

  // Data — two MACs per DSP
  input  int8_t   weight_0,      // MAC 0 weight
  input  int8_t   weight_1,      // MAC 1 weight
  input  int8_t   activation_0,  // MAC 0 activation
  input  int8_t   activation_1,  // MAC 1 activation
  output int32_t  accumulator_0, // MAC 0 accumulator
  output int32_t  accumulator_1, // MAC 1 accumulator
  output logic    valid_out
);

  // ── Pipeline registers ─────────────────────────────────────
  int16_t product_0_r, product_1_r;
  logic   valid_p1;

  // ── Stage 1: Multiply (maps to MULT18X18D) ────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      product_0_r <= '0;
      product_1_r <= '0;
      valid_p1    <= 1'b0;
    end else begin
      product_0_r <= int16_t'(weight_0) * int16_t'(activation_0);
      product_1_r <= int16_t'(weight_1) * int16_t'(activation_1);
      valid_p1    <= valid_in;
    end
  end

  // ── Stage 2: Accumulate ────────────────────────────────────
  int32_t acc_0_r, acc_1_r;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      acc_0_r   <= '0;
      acc_1_r   <= '0;
      valid_out <= 1'b0;
    end else if (clear_acc) begin
      acc_0_r   <= '0;
      acc_1_r   <= '0;
      valid_out <= 1'b0;
    end else begin
      if (valid_p1) begin
        acc_0_r <= acc_0_r + int32_t'(product_0_r);
        acc_1_r <= acc_1_r + int32_t'(product_1_r);
      end
      valid_out <= valid_p1;
    end
  end

  assign accumulator_0 = acc_0_r;
  assign accumulator_1 = acc_1_r;

endmodule
