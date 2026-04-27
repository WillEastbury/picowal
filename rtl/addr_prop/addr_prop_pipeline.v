// addr_prop_pipeline.v — N-stage address propagation chain
//
// Chains N addr_prop_stage modules into a deterministic pipeline.
//
// LATENCY: exactly N × 2 clocks (each stage = 2 clocks: input reg + BRAM read reg)
// THROUGHPUT: 1 address per clock (fully pipelined)
//
// Mental model:
//   The address IS the state.
//   Memory defines the transition function.
//   Each stage applies one transition.
//   N stages = N transitions = exactly N×2 ticks.
//
//   Input → [table 0] → [table 1] → [table 2] → ... → [table N-1] → Output
//
// This is a clocked address-rewriting machine. NOT a CPU.

module addr_prop_pipeline #(
    parameter ADDR_W   = 16,    // address/state width
    parameter N_STAGES = 4      // number of pipeline stages
)(
    input  wire                clk,
    input  wire                rst_n,

    // --- Streaming interface ---
    input  wire                valid_in,
    input  wire [ADDR_W-1:0]  addr_in,

    output wire                valid_out,
    output wire [ADDR_W-1:0]  addr_out,

    // --- Table load interface (active during LOAD mode) ---
    input  wire                load_en,
    input  wire [$clog2(N_STAGES)-1:0] load_stage,  // which stage to load
    input  wire [ADDR_W-1:0]  load_addr,
    input  wire [ADDR_W-1:0]  load_data,
    input  wire                load_we,

    // --- Instrumentation ---
    output reg  [31:0]         cnt_cycles,       // total clock cycles since reset
    output reg  [31:0]         cnt_addresses,    // addresses that completed pipeline
    output wire [ADDR_W-1:0]  dbg_stage_addr [0:N_STAGES-1]  // per-stage debug
);

    // =====================================================================
    // Inter-stage wiring
    // =====================================================================

    wire [ADDR_W-1:0] stage_addr [0:N_STAGES];
    wire              stage_valid [0:N_STAGES];

    // Input connects to first stage
    assign stage_addr[0]  = addr_in;
    assign stage_valid[0] = valid_in;

    // Output from last stage
    assign addr_out   = stage_addr[N_STAGES];
    assign valid_out  = stage_valid[N_STAGES];

    // =====================================================================
    // Instantiate N stages — each is an independent lookup table
    // =====================================================================

    genvar s;
    generate
        for (s = 0; s < N_STAGES; s = s + 1) begin : stage

            // Per-stage load enable: only the selected stage accepts writes
            wire stage_load_en = load_en && (load_stage == s);

            addr_prop_stage #(
                .ADDR_W   (ADDR_W),
                .STAGE_ID (s)
            ) u_stage (
                .clk        (clk),
                .rst_n      (rst_n),

                .valid_in   (stage_valid[s]),
                .addr_in    (stage_addr[s]),
                .valid_out  (stage_valid[s+1]),
                .addr_out   (stage_addr[s+1]),

                .load_en    (stage_load_en),
                .load_addr  (load_addr),
                .load_data  (load_data),
                .load_we    (load_we),

                .dbg_last_addr (dbg_stage_addr[s])
            );
        end
    endgenerate

    // =====================================================================
    // Instrumentation counters
    // =====================================================================

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cnt_cycles    <= 32'd0;
            cnt_addresses <= 32'd0;
        end else begin
            cnt_cycles <= cnt_cycles + 1;
            if (valid_out)
                cnt_addresses <= cnt_addresses + 1;
        end
    end

endmodule
