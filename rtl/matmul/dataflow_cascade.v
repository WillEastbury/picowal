// dataflow_cascade.v — Multi-layer neural network as pure hardware cascade
//
// ZERO SOFTWARE IN THE INFERENCE PATH.
//
// Data flows through SRAM chips like water through pipes:
//
//   Input ──▶ [SRAM bank 0] ──▶ adder+ReLU ──▶ [SRAM bank 1] ──▶ adder+ReLU ──▶ Output
//              Layer 0 weights                    Layer 1 weights
//
// Each SRAM bank = 8 chips with shared address bus
// Each adder+ReLU = ~150 LUTs in FPGA
// Output of each stage drives address pins of next stage DIRECTLY
//
// 3-layer example (8→8→8→8 network):
//   SRAM chips: 3 × 8 = 24
//   FPGA LUTs:  3 × 150 = 450 (fits in HX1K!)
//   Latency:    3 × 17ns = 51ns total
//   Throughput: 59M inf/s pipelined
//   Cost:       24 × $1.70 + $3.50 FPGA = ~$44
//
// For wider layers (e.g. 8→64→64→10):
//   Layer 0: 8 SRAMs, 8 output neurons → 8 sequential neuron_sel cycles
//   Layer 1: 8 SRAMs, 64 output neurons → 64 sequential cycles (time-mux)
//   Layer 2: 8 SRAMs, 10 output neurons → 10 sequential cycles
//   With pipelining: all layers active simultaneously
//
// Weight loading: RP2354B fills SRAMs via shared write bus at startup.
// During inference: RP is IDLE. Hardware does everything.

module dataflow_cascade #(
    parameter N_LAYERS = 3,
    parameter N_INPUTS = 8,      // width of each dot product (SRAM chips per bank)
    parameter [N_LAYERS*4-1:0] SHIFTS = {4'd3, 4'd3, 4'd3}  // per-layer shift
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- Inference input ----
    input  wire [7:0]  x_in,
    input  wire        start,

    // ---- SRAM data buses for ALL layers ----
    // Layer L, SRAM chip S: sram_data[L][S]
    input  wire [15:0] sram_data [0:N_LAYERS-1][0:N_INPUTS-1],

    // ---- Neuron/row select for each layer (active to SRAM addr[17:8]) ----
    output reg  [9:0]  neuron_sel [0:N_LAYERS-1],

    // ---- Address output for each layer (active to SRAM addr[7:0]) ----
    output wire [7:0]  layer_addr [0:N_LAYERS-1],

    // ---- Final output ----
    output wire [7:0]  y_out,
    output wire        y_valid,

    // ---- Weight loading (active to all SRAMs via shared bus) ----
    output reg  [17:0] wl_addr,
    output reg  [15:0] wl_data,
    output reg         wl_we,
    output reg  [1:0]  wl_layer_sel,   // which layer's SRAMs get the write
    output reg  [2:0]  wl_sram_sel,    // which SRAM within the layer

    // ---- Status ----
    output wire        busy,
    output wire [18:0] debug_raw [0:N_LAYERS-1]
);

    // =====================================================================
    // Wire up the cascade: output of layer N → input of layer N+1
    // =====================================================================

    wire [7:0]  inter_data [0:N_LAYERS];   // inter-layer activations
    wire        inter_valid [0:N_LAYERS];  // inter-layer valid strobes

    // Input feeds layer 0
    assign inter_data[0]  = x_in;
    assign inter_valid[0] = start;
    assign layer_addr[0]  = x_in;

    genvar l;
    generate
        for (l = 0; l < N_LAYERS; l = l + 1) begin : layer

            wire [7:0] layer_out;
            wire       layer_out_valid;

            dataflow_layer #(
                .N_INPUTS   (N_INPUTS),
                .SHIFT_BITS (SHIFTS[l*4 +: 4])
            ) dl (
                .clk         (clk),
                .x_in        (inter_data[l]),
                .x_valid     (inter_valid[l]),
                .sram_d      (sram_data[l]),
                .neuron_sel  (neuron_sel[l]),
                .y_out       (layer_out),
                .y_valid     (layer_out_valid),
                .raw_sum     (debug_raw[l]),
                .relu_active ()
            );

            assign inter_data[l+1]  = layer_out;
            assign inter_valid[l+1] = layer_out_valid;

            // Layer output DIRECTLY drives next layer's SRAM address pins
            if (l < N_LAYERS - 1) begin
                assign layer_addr[l+1] = layer_out;
            end
        end
    endgenerate

    // Final output
    assign y_out   = inter_data[N_LAYERS];
    assign y_valid = inter_valid[N_LAYERS];

    // =====================================================================
    // Neuron sequencer: cycles through output neurons for each layer
    //
    // For single-output-neuron-at-a-time (simplest):
    //   Each layer computes one output neuron per cycle
    //   neuron_sel drives SRAM A[17:8] to select the weight row
    //
    // For a layer with M output neurons:
    //   Cycle through neuron_sel = 0, 1, 2, ..., M-1
    //   Collect M results, then feed all M to next layer
    //
    // For 8→8→8 (all layers width 8):
    //   Each layer: 8 cycles. Pipeline overlap between layers.
    //   Total: 8 + pipeline_depth cycles for full inference.
    // =====================================================================

    // Simple sequencer: all layers share the same neuron counter
    // (works for equal-width layers; extend for variable widths)
    reg [9:0] neuron_count;
    reg       running;
    reg [3:0] pipe_count;

    assign busy = running;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            running      <= 0;
            neuron_count <= 0;
            pipe_count   <= 0;
        end else if (start && !running) begin
            running      <= 1;
            neuron_count <= 0;
            pipe_count   <= 0;
        end else if (running) begin
            neuron_count <= neuron_count + 1;

            // Update all layer neuron selects
            // (in a real design, each layer has its own counter offset)
            neuron_sel[0] <= neuron_count;
            if (N_LAYERS > 1) neuron_sel[1] <= neuron_count;
            if (N_LAYERS > 2) neuron_sel[2] <= neuron_count;

            // Count output valids to know when done
            if (inter_valid[N_LAYERS])
                pipe_count <= pipe_count + 1;

            // Done when we've collected enough outputs
            if (pipe_count >= N_INPUTS) begin
                running <= 0;
            end
        end
    end

    // =====================================================================
    // Weight loading FSM
    // RP calls this at startup to fill all SRAM lookup tables
    // Not in the inference path — runs once at model load time
    // =====================================================================

    // Weight loading is directly controlled by RP2354B via PIO.
    // The wl_* outputs go to tristate muxes that switch SRAM buses
    // from dataflow mode to write mode. See board-level design.

endmodule
