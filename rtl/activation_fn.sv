// activation_fn.sv — Post-accumulator activation function unit
// Supports: ReLU, bias add, requantize (INT32 accumulator → INT8 output)
// Pipeline: bias_add → relu → requant → saturate

module activation_fn
  import hydra_infer_pkg::*;
#(
  parameter int N = ARRAY_N
)(
  input  logic    clk,
  input  logic    rst_n,

  // Control (from layer descriptor)
  input  logic                     relu_en,
  input  logic                     bias_en,
  input  logic [SCALE_W-1:0]      requant_scale,   // fixed-point multiplier
  input  logic [SHIFT_W-1:0]      requant_shift,   // right-shift amount

  // Bias input (one per output column)
  input  int32_t                   bias_in [N],

  // Input: accumulated partial sums from systolic array
  input  logic                     in_valid,
  input  int32_t                   acc_in [N],

  // Output: quantized INT8 activations
  output logic                     out_valid,
  output int8_t                    act_out [N]
);

  // ── Stage 1: Bias add ──────────────────────────────────────
  int32_t biased [N];
  logic   s1_valid;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      s1_valid <= 1'b0;
      for (int i = 0; i < N; i++)
        biased[i] <= '0;
    end else begin
      s1_valid <= in_valid;
      for (int i = 0; i < N; i++)
        biased[i] <= bias_en ? (acc_in[i] + bias_in[i]) : acc_in[i];
    end
  end

  // ── Stage 2: ReLU ──────────────────────────────────────────
  int32_t relu_out [N];
  logic   s2_valid;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      s2_valid <= 1'b0;
      for (int i = 0; i < N; i++)
        relu_out[i] <= '0;
    end else begin
      s2_valid <= s1_valid;
      for (int i = 0; i < N; i++) begin
        if (relu_en && biased[i] < 0)
          relu_out[i] <= '0;
        else
          relu_out[i] <= biased[i];
      end
    end
  end

  // ── Stage 3: Requantize (scale + shift + saturate) ─────────
  // out = clamp( (val * scale) >> shift, -128, 127 )
  logic   s3_valid;
  int8_t  quant_out [N];

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      s3_valid <= 1'b0;
      for (int i = 0; i < N; i++)
        quant_out[i] <= '0;
    end else begin
      s3_valid <= s2_valid;
      for (int i = 0; i < N; i++) begin
        // Multiply by scale (widening)
        logic signed [47:0] scaled;
        scaled = int32_t'(relu_out[i]) * $signed({1'b0, requant_scale});
        // Arithmetic right shift
        logic signed [47:0] shifted;
        shifted = scaled >>> requant_shift;
        // Saturate to INT8 range
        if (shifted > 127)
          quant_out[i] <= int8_t'(127);
        else if (shifted < -128)
          quant_out[i] <= int8_t'(-128);
        else
          quant_out[i] <= int8_t'(shifted[7:0]);
      end
    end
  end

  assign out_valid = s3_valid;
  assign act_out   = quant_out;

endmodule
