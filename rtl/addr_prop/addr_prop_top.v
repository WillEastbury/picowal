// addr_prop_top.v — Top-level address propagation engine
//
// Two modes:
//   LOAD: Host writes transition tables into each stage's BRAM
//   RUN:  Pipeline runs autonomously — no CPU involvement
//
// Mode control is a single wire. No FSM. No instruction decode.
// The FPGA is just clocked wiring + BRAM.
//
// ┌──────────────────────────────────────────────────────┐
// │                addr_prop_top                         │
// │                                                      │
// │  mode_run=0 (LOAD):                                 │
// │    Host writes tables via load_* interface           │
// │    Pipeline is idle                                  │
// │                                                      │
// │  mode_run=1 (RUN):                                  │
// │    Host feeds addresses via addr_in                  │
// │    Pipeline propagates autonomously                  │
// │    Results appear at addr_out after PIPELINE_LATENCY │
// │    clocks. Always. No exceptions.                    │
// │                                                      │
// │  PIPELINE_LATENCY = N_STAGES × 2 clocks             │
// │  (each stage: 1 clk input reg + 1 clk BRAM read)    │
// └──────────────────────────────────────────────────────┘

module addr_prop_top #(
    parameter ADDR_W   = 16,
    parameter N_STAGES = 4
)(
    input  wire                clk,
    input  wire                rst_n,

    // --- Mode control ---
    input  wire                mode_run,     // 0=LOAD, 1=RUN

    // --- Run-mode streaming interface ---
    input  wire                run_valid,
    input  wire [ADDR_W-1:0]  run_addr_in,
    output wire                run_valid_out,
    output wire [ADDR_W-1:0]  run_addr_out,

    // --- Load-mode interface ---
    input  wire [$clog2(N_STAGES)-1:0] load_stage,
    input  wire [ADDR_W-1:0]  load_addr,
    input  wire [ADDR_W-1:0]  load_data,
    input  wire                load_we,

    // --- Instrumentation ---
    output wire [31:0]         cnt_cycles,
    output wire [31:0]         cnt_addresses,

    // --- Debug: per-stage current address ---
    output wire [ADDR_W-1:0]  dbg_stage_addr [0:N_STAGES-1]
);

    // Pipeline only accepts input in RUN mode
    wire pipe_valid_in = mode_run & run_valid;
    wire pipe_load_en  = ~mode_run;

    addr_prop_pipeline #(
        .ADDR_W   (ADDR_W),
        .N_STAGES (N_STAGES)
    ) u_pipeline (
        .clk           (clk),
        .rst_n         (rst_n),

        .valid_in      (pipe_valid_in),
        .addr_in       (run_addr_in),
        .valid_out     (run_valid_out),
        .addr_out      (run_addr_out),

        .load_en       (pipe_load_en),
        .load_stage    (load_stage),
        .load_addr     (load_addr),
        .load_data     (load_data),
        .load_we       (load_we),

        .cnt_cycles    (cnt_cycles),
        .cnt_addresses (cnt_addresses),
        .dbg_stage_addr(dbg_stage_addr)
    );

endmodule
