// dataflow_layer.v — One neural network layer as pure hardware dataflow
//
// NO CPU. NO INSTRUCTIONS. NO OVERHEAD.
//
// Input byte → SRAM address pins → products on data pins →
// adder tree → ReLU → requantize to 8-bit → DIRECTLY to next layer's address pins
//
// The weights ARE the hardware. Data propagates at wire speed.
//
// Each layer:
//   - 8 SRAM chips (shared address bus = broadcast input)
//   - Adder tree in FPGA (~120 LUTs)
//   - ReLU: if negative, clamp to 0 (~4 LUTs)
//   - Requantize: 19-bit → 8-bit with learned shift/scale (~20 LUTs)
//   - Output drives NEXT layer's SRAM address pins directly
//
// Latency per layer: 10ns (SRAM) + 5ns (adder) + 2ns (ReLU+quant) = ~17ns
// Throughput: fully pipelined — 1 inference per layer delay
//
// For a 3-layer network cascaded in hardware:
//   Total latency: 3 × 17ns = 51ns per inference
//   Throughput: ~59M inferences/sec (pipelined)
//   Cost: 24 SRAM chips = ~£28

module dataflow_layer #(
    parameter N_INPUTS   = 8,    // dot product width (= number of SRAM chips)
    parameter SHIFT_BITS = 3     // right-shift for requantization (set per layer)
)(
    // No clock for the dataflow path! (only for pipeline registers)
    input  wire        clk,

    // ---- Input: directly from previous layer or external ----
    input  wire [7:0]  x_in,           // 8-bit activation input
    input  wire        x_valid,        // data valid strobe

    // ---- SRAM data buses (active to FPGA pins from 8 SRAM chips) ----
    // Address bus is active: x_in active to SRAM A[7:0] pins via PCB traces
    // Upper address bits active to SRAM A[17:8] for neuron/row selection
    input  wire [15:0] sram_d [0:N_INPUTS-1],

    // ---- Row select (which output neuron) ----
    input  wire [9:0]  neuron_sel,     // drives SRAM A[17:8] via PCB

    // ---- Output: drives next layer's SRAM address pins ----
    output wire [7:0]  y_out,          // requantized 8-bit output
    output wire        y_valid,        // valid strobe for next layer

    // ---- Monitoring ----
    output wire [18:0] raw_sum,        // pre-activation value (for debug)
    output wire        relu_active     // 1 if ReLU passed (positive)
);

    // =====================================================================
    // Stage 1: Adder tree — reduce 8 × 16-bit products to 19-bit sum
    // Products already on SRAM data pins (async, 10ns after address)
    // =====================================================================

    // Combinational adder tree — NO clock, pure wires
    // Stage 1: 8 → 4
    wire signed [16:0] s1_0 = $signed(sram_d[0]) + $signed(sram_d[1]);
    wire signed [16:0] s1_1 = $signed(sram_d[2]) + $signed(sram_d[3]);
    wire signed [16:0] s1_2 = $signed(sram_d[4]) + $signed(sram_d[5]);
    wire signed [16:0] s1_3 = $signed(sram_d[6]) + $signed(sram_d[7]);

    // Stage 2: 4 → 2
    wire signed [17:0] s2_0 = {s1_0[16], s1_0} + {s1_1[16], s1_1};
    wire signed [17:0] s2_1 = {s1_2[16], s1_2} + {s1_3[16], s1_3};

    // Stage 3: 2 → 1
    wire signed [18:0] dot_sum = {s2_0[17], s2_0} + {s2_1[17], s2_1};

    assign raw_sum = dot_sum;

    // =====================================================================
    // Stage 2: ReLU activation — if negative, clamp to zero
    // Pure combinational. ~4 LUTs.
    // =====================================================================

    wire signed [18:0] activated = (dot_sum[18]) ? 19'sd0 : dot_sum;
    assign relu_active = ~dot_sum[18];

    // =====================================================================
    // Stage 3: Requantize 19-bit → 8-bit
    // Right-shift by learned amount + saturate to INT8 range
    // Pure combinational. ~20 LUTs.
    //
    // SHIFT_BITS is set per-layer at synthesis time based on weight scale.
    // This replaces batch normalization in hardware.
    // =====================================================================

    wire signed [18:0] shifted = activated >>> SHIFT_BITS;

    // Saturate to unsigned 8-bit (post-ReLU, always non-negative)
    wire [7:0] saturated = (shifted > 19'sd255) ? 8'd255 : shifted[7:0];

    assign y_out = saturated;

    // =====================================================================
    // Pipeline register — optional, for timing closure on long chains
    // Can be bypassed for minimum latency (pure combinational path)
    // =====================================================================

    reg y_valid_r;
    always @(posedge clk) y_valid_r <= x_valid;
    assign y_valid = y_valid_r;

endmodule
