// dataflow_network.v — Complete inference engine: NIC → SRAM cascade → result
//
// THE COMPLETE PICTURE:
//
//        ┌─────────┐
//   NIC ─┤ RP2354B ├─── A[17:0] shared bus ─────────────────────────────────
//        │ (PIO)   │    (active to SRAM addr pins via PCB traces)
//        └────┬────┘
//             │ loads weights at startup
//             │ sets neuron_sel during inference
//             │ otherwise IDLE — hardware does the rest
//             │
//     ════════╪══════════════════════════════════════════════════════
//     Layer 0 │              Layer 1                Layer 2
//     ════════╪══════════════════════════════════════════════════════
//             │
//     ┌───────▼───────┐     ┌───────────────┐     ┌───────────────┐
//     │ 8× SRAM bank  │     │ 8× SRAM bank  │     │ 8× SRAM bank  │
//     │  A[7:0]=input  │     │  A[7:0]=act1   │     │  A[7:0]=act2   │
//     │  D[15:0]=prod  │     │  D[15:0]=prod  │     │  D[15:0]=prod  │
//     └───┬──┬──┬──┬───┘     └───┬──┬──┬──┬───┘     └───┬──┬──┬──┬───┘
//         │  │  │  │ 8×16b       │  │  │  │              │  │  │  │
//         ▼  ▼  ▼  ▼             ▼  ▼  ▼  ▼              ▼  ▼  ▼  ▼
//     ┌────────────────┐     ┌────────────────┐     ┌────────────────┐
//     │ adder tree     │     │ adder tree     │     │ adder tree     │
//     │ ReLU           │     │ ReLU           │     │ (no ReLU -     │
//     │ requant → 8bit │     │ requant → 8bit │     │  raw output)   │
//     └───────┬────────┘     └───────┬────────┘     └───────┬────────┘
//             │ 8-bit                │ 8-bit                │ 19-bit
//             │                      │                      │
//             ▼                      ▼                      ▼
//     DIRECTLY TO             DIRECTLY TO              RESULT TO RP
//     LAYER 1 SRAM            LAYER 2 SRAM
//     A[7:0] PINS             A[7:0] PINS
//
//     ════════════════════════════════════════════════════════════════
//
//     Total propagation delay: 3 × 17ns = 51ns
//     No clock in the multiply path. No CPU. No instructions.
//     Data flows through SRAM at the speed of physics.
//
// This file is the documentation / top-level wrapper.
// Active Verilog is in dataflow_cascade.v and dataflow_layer.v.

module dataflow_network #(
    parameter N_LAYERS    = 3,
    parameter LAYER_WIDTH = 8      // neurons per layer (dot product width)
)(
    input  wire        clk,        // 133MHz system clock (for sequencing + weight load)
    input  wire        rst_n,

    // ---- From NIC/RP: inference input ----
    input  wire [7:0]  input_byte,
    input  wire        input_valid,

    // ---- Physical SRAM data buses (active to FPGA pins) ----
    // 3 layers × 8 SRAM chips = 24 × 16-bit data buses = 384 FPGA pins
    //
    // On HX8K-CT256 (208 GPIO): can fit 1 layer internally
    // For 3 layers: use 3× HX4K-TQ144 (107 GPIO each) — one per layer
    // OR: 1× LIFCL-17 (QFN-72) handles all 3 adder trees, address muxing
    //     via time-multiplexed shared data bus
    //
    // For this reference design: expose all buses directly
    input  wire [15:0] l0_sram_d [0:LAYER_WIDTH-1],
    input  wire [15:0] l1_sram_d [0:LAYER_WIDTH-1],
    input  wire [15:0] l2_sram_d [0:LAYER_WIDTH-1],

    // ---- SRAM address outputs (active to SRAM addr pins via PCB) ----
    output wire [7:0]  l0_addr,    // layer 0 input address
    output wire [7:0]  l1_addr,    // layer 1 input address (from layer 0 output)
    output wire [7:0]  l2_addr,    // layer 2 input address (from layer 1 output)

    // ---- Row/neuron select (active to SRAM A[17:8]) ----
    output wire [9:0]  l0_neuron,
    output wire [9:0]  l1_neuron,
    output wire [9:0]  l2_neuron,

    // ---- Inference output ----
    output wire [7:0]  result,
    output wire        result_valid,

    // ---- Weight loading (active from RP via PIO) ----
    input  wire [17:0] wl_addr,
    input  wire [15:0] wl_data,
    input  wire        wl_we,
    input  wire [1:0]  wl_layer,
    input  wire [2:0]  wl_sram,

    // ---- Status ----
    output wire        busy
);

    // Pack SRAM data into 2D array for cascade module
    wire [15:0] all_sram [0:N_LAYERS-1][0:LAYER_WIDTH-1];

    genvar s;
    generate
        for (s = 0; s < LAYER_WIDTH; s = s + 1) begin : pack
            assign all_sram[0][s] = l0_sram_d[s];
            assign all_sram[1][s] = l1_sram_d[s];
            assign all_sram[2][s] = l2_sram_d[s];
        end
    endgenerate

    // Neuron select buses
    wire [9:0] neuron_sel [0:N_LAYERS-1];
    assign l0_neuron = neuron_sel[0];
    assign l1_neuron = neuron_sel[1];
    assign l2_neuron = neuron_sel[2];

    // Address buses
    wire [7:0] layer_addr [0:N_LAYERS-1];
    assign l0_addr = layer_addr[0];
    assign l1_addr = layer_addr[1];
    assign l2_addr = layer_addr[2];

    // Debug
    wire [18:0] debug_raw [0:N_LAYERS-1];

    dataflow_cascade #(
        .N_LAYERS (N_LAYERS),
        .N_INPUTS (LAYER_WIDTH),
        .SHIFTS   ({4'd3, 4'd3, 4'd3})
    ) cascade (
        .clk          (clk),
        .rst_n        (rst_n),
        .x_in         (input_byte),
        .start        (input_valid),
        .sram_data    (all_sram),
        .neuron_sel   (neuron_sel),
        .layer_addr   (layer_addr),
        .y_out        (result),
        .y_valid      (result_valid),
        .wl_addr      (),    // directly from RP
        .wl_data      (),
        .wl_we        (),
        .wl_layer_sel (),
        .wl_sram_sel  (),
        .busy         (busy),
        .debug_raw    (debug_raw)
    );

endmodule
