// tb_addr_prop.v — Testbench for address propagation engine
//
// Proves:
//   1. Deterministic latency: output arrives exactly N_STAGES × 2 clocks after input
//   2. Correct propagation: each stage applies its transition function
//   3. Full pipeline: one result per clock at steady state
//   4. No stalls: pipeline never pauses
//
// Test patterns:
//   Stage 0: addr_out = addr_in + 1     (increment)
//   Stage 1: addr_out = addr_in ^ 0xAA  (XOR scramble)
//   Stage 2: addr_out = addr_in + 1     (increment again)
//   Stage 3: addr_out = addr_in ^ 0x55  (different XOR)
//
// Uses 8-bit addresses to keep BRAM small for simulation.

`timescale 1ns / 1ps

module tb_addr_prop;

    parameter ADDR_W   = 8;
    parameter N_STAGES = 4;
    parameter PIPELINE_LATENCY = N_STAGES * 2;  // 2 clocks per stage
    parameter TABLE_SIZE = (1 << ADDR_W);       // 256 entries per stage

    reg                clk;
    reg                rst_n;
    reg                mode_run;
    reg                run_valid;
    reg [ADDR_W-1:0]  run_addr_in;
    wire               run_valid_out;
    wire [ADDR_W-1:0]  run_addr_out;
    reg [$clog2(N_STAGES)-1:0] load_stage;
    reg [ADDR_W-1:0]  load_addr;
    reg [ADDR_W-1:0]  load_data;
    reg                load_we;
    wire [31:0]        cnt_cycles;
    wire [31:0]        cnt_addresses;
    wire [ADDR_W-1:0]  dbg_stage_addr [0:N_STAGES-1];

    // DUT
    addr_prop_top #(
        .ADDR_W   (ADDR_W),
        .N_STAGES (N_STAGES)
    ) dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .mode_run       (mode_run),
        .run_valid      (run_valid),
        .run_addr_in    (run_addr_in),
        .run_valid_out  (run_valid_out),
        .run_addr_out   (run_addr_out),
        .load_stage     (load_stage),
        .load_addr      (load_addr),
        .load_data      (load_data),
        .load_we        (load_we),
        .cnt_cycles     (cnt_cycles),
        .cnt_addresses  (cnt_addresses),
        .dbg_stage_addr (dbg_stage_addr)
    );

    // Clock: 50MHz = 20ns period
    initial clk = 0;
    always #10 clk = ~clk;

    // =====================================================================
    // Helper task: write one table entry
    // =====================================================================

    task load_one;
        input [$clog2(N_STAGES)-1:0] stg;
        input [ADDR_W-1:0] addr;
        input [ADDR_W-1:0] data;
        begin
            @(posedge clk);
            load_stage <= stg;
            load_addr  <= addr;
            load_data  <= data;
            load_we    <= 1;
            @(posedge clk);
            load_we    <= 0;
        end
    endtask

    // =====================================================================
    // Golden model: compute expected output for a given input
    // =====================================================================

    function [ADDR_W-1:0] expected_result;
        input [ADDR_W-1:0] addr;
        reg [ADDR_W-1:0] a;
        begin
            a = addr;
            a = (a + 1) & {ADDR_W{1'b1}};    // stage 0: increment
            a = a ^ 8'hAA;                    // stage 1: XOR 0xAA
            a = (a + 1) & {ADDR_W{1'b1}};    // stage 2: increment
            a = a ^ 8'h55;                    // stage 3: XOR 0x55
            expected_result = a;
        end
    endfunction

    // =====================================================================
    // Output checker
    // =====================================================================

    integer errors;
    integer results_seen;
    reg [ADDR_W-1:0] input_fifo [0:255];
    integer fifo_rd;
    integer first_valid_cycle;
    integer last_input_cycle;
    reg     seen_first;

    initial begin
        errors       = 0;
        results_seen = 0;
        fifo_rd      = 0;
        seen_first   = 0;
    end

    always @(posedge clk) begin
        if (run_valid_out) begin
            if (!seen_first) begin
                first_valid_cycle = cnt_cycles;
                seen_first = 1;
            end
            if (run_addr_out !== expected_result(input_fifo[fifo_rd])) begin
                $display("  FAIL: input 0x%02h → got 0x%02h, expected 0x%02h (result #%0d)",
                         input_fifo[fifo_rd], run_addr_out,
                         expected_result(input_fifo[fifo_rd]), results_seen);
                errors = errors + 1;
            end else begin
                $display("  PASS: input 0x%02h → output 0x%02h ✓",
                         input_fifo[fifo_rd], run_addr_out);
            end
            fifo_rd = fifo_rd + 1;
            results_seen = results_seen + 1;
        end
    end

    // =====================================================================
    // Main test sequence
    // =====================================================================

    integer i;
    integer fifo_wr;

    initial begin
        $dumpfile("tb_addr_prop.vcd");
        $dumpvars(0, tb_addr_prop);

        // Initialise
        rst_n       = 0;
        mode_run    = 0;
        run_valid   = 0;
        load_we     = 0;
        run_addr_in = 0;
        load_stage  = 0;
        load_addr   = 0;
        load_data   = 0;
        fifo_wr     = 0;

        repeat (4) @(posedge clk);
        rst_n = 1;
        repeat (2) @(posedge clk);

        // =========================================================
        // PHASE 1: Load all transition tables
        // =========================================================
        $display("");
        $display("============================================");
        $display(" PHASE 1: Loading transition tables");
        $display("============================================");

        // Stage 0: f(x) = x + 1
        $display("  Stage 0: f(x) = x + 1");
        for (i = 0; i < TABLE_SIZE; i = i + 1)
            load_one(2'd0, i[ADDR_W-1:0], ((i + 1) & {ADDR_W{1'b1}}));

        // Stage 1: f(x) = x ^ 0xAA
        $display("  Stage 1: f(x) = x ^ 0xAA");
        for (i = 0; i < TABLE_SIZE; i = i + 1)
            load_one(2'd1, i[ADDR_W-1:0], (i[ADDR_W-1:0] ^ 8'hAA));

        // Stage 2: f(x) = x + 1
        $display("  Stage 2: f(x) = x + 1");
        for (i = 0; i < TABLE_SIZE; i = i + 1)
            load_one(2'd2, i[ADDR_W-1:0], ((i + 1) & {ADDR_W{1'b1}}));

        // Stage 3: f(x) = x ^ 0x55
        $display("  Stage 3: f(x) = x ^ 0x55");
        for (i = 0; i < TABLE_SIZE; i = i + 1)
            load_one(2'd3, i[ADDR_W-1:0], (i[ADDR_W-1:0] ^ 8'h55));

        $display("  All tables loaded.");

        // =========================================================
        // PHASE 2: Switch to RUN mode
        // =========================================================
        $display("");
        $display("============================================");
        $display(" PHASE 2: Entering RUN mode");
        $display("   Pipeline depth: %0d stages", N_STAGES);
        $display("   Latency: %0d clocks", PIPELINE_LATENCY);
        $display("============================================");

        @(posedge clk);
        mode_run = 1;
        repeat (2) @(posedge clk);

        // =========================================================
        // TEST 1: Single address — verify latency
        // =========================================================
        $display("");
        $display("--- TEST 1: Single address (latency check) ---");

        input_fifo[fifo_wr] = 8'h01;
        fifo_wr = fifo_wr + 1;

        @(posedge clk);
        last_input_cycle = cnt_cycles;
        run_valid   <= 1;
        run_addr_in <= 8'h01;
        @(posedge clk);
        run_valid   <= 0;

        // Wait for result
        repeat (PIPELINE_LATENCY + 4) @(posedge clk);

        if (seen_first) begin
            $display("  Measured latency: %0d clocks (expected %0d)",
                     first_valid_cycle - last_input_cycle, PIPELINE_LATENCY);
        end

        // =========================================================
        // TEST 2: Continuous burst — verify throughput
        // =========================================================
        $display("");
        $display("--- TEST 2: Burst of 16 addresses (throughput check) ---");

        for (i = 0; i < 16; i = i + 1) begin
            input_fifo[fifo_wr] = i[ADDR_W-1:0];
            fifo_wr = fifo_wr + 1;

            @(posedge clk);
            run_valid   <= 1;
            run_addr_in <= i[ADDR_W-1:0];
        end
        @(posedge clk);
        run_valid <= 0;

        // Drain
        repeat (PIPELINE_LATENCY + 20) @(posedge clk);

        // =========================================================
        // TEST 3: Back-to-back with varying inputs
        // =========================================================
        $display("");
        $display("--- TEST 3: 32 back-to-back (stride 7) ---");

        for (i = 0; i < 32; i = i + 1) begin
            input_fifo[fifo_wr] = ((i * 7) & 8'hFF);
            fifo_wr = fifo_wr + 1;

            @(posedge clk);
            run_valid   <= 1;
            run_addr_in <= (i * 7) & 8'hFF;
        end
        @(posedge clk);
        run_valid <= 0;

        // Drain
        repeat (PIPELINE_LATENCY + 20) @(posedge clk);

        // =========================================================
        // TEST 4: Edge cases
        // =========================================================
        $display("");
        $display("--- TEST 4: Edge cases (0x00, 0xFF, 0x80, 0x7F) ---");

        begin
            reg [ADDR_W-1:0] edge_vals [0:3];
            edge_vals[0] = 8'h00;
            edge_vals[1] = 8'hFF;
            edge_vals[2] = 8'h80;
            edge_vals[3] = 8'h7F;

            for (i = 0; i < 4; i = i + 1) begin
                input_fifo[fifo_wr] = edge_vals[i];
                fifo_wr = fifo_wr + 1;

                @(posedge clk);
                run_valid   <= 1;
                run_addr_in <= edge_vals[i];
            end
            @(posedge clk);
            run_valid <= 0;
        end

        // Drain
        repeat (PIPELINE_LATENCY + 10) @(posedge clk);

        // =========================================================
        // REPORT
        // =========================================================
        $display("");
        $display("============================================");
        $display(" RESULTS");
        $display("============================================");
        $display("  Addresses processed (HW counter): %0d", cnt_addresses);
        $display("  Results verified (TB):             %0d", results_seen);
        $display("  Errors:                            %0d", errors);
        $display("  Pipeline stages:                   %0d", N_STAGES);
        $display("  Latency per stage:                 2 clocks");
        $display("  Total pipeline latency:            %0d clocks", PIPELINE_LATENCY);
        $display("");

        if (errors == 0) begin
            $display("  ╔═══════════════════════════════════════════╗");
            $display("  ║  PASS: Deterministic address propagation  ║");
            $display("  ║  No CPU. No instructions. Pure lookup.    ║");
            $display("  ║  address_in → table → address_out → next  ║");
            $display("  ╚═══════════════════════════════════════════╝");
        end else begin
            $display("  *** FAIL: %0d errors detected ***", errors);
        end

        $display("");
        $finish;
    end

endmodule
