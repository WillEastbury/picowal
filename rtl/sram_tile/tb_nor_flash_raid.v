// tb_nor_flash_raid.v — Testbench for NOR Flash RAID-0 controller
//
// Uses simplified flash model (async SRAM-like behavior for reads,
// instant writes for simulation speed).

`timescale 1ns / 1ps

module tb_nor_flash_raid;

    parameter N_CHIPS  = 4;          // small for sim
    parameter FLASH_AW = 8;          // 256 addresses for testing
    parameter FLASH_DW = 16;
    parameter BLOCK_W  = N_CHIPS * FLASH_DW;

    reg                  clk;
    reg                  rst_n;
    reg  [FLASH_AW-1:0] addr;
    reg                  rw_n;
    reg                  start;
    wire                 ready;
    reg  [BLOCK_W-1:0]  din;
    wire [BLOCK_W-1:0]  dout;
    wire                 dout_valid;

    wire [FLASH_AW-1:0]        flash_a;
    wire [FLASH_DW-1:0]        flash_dq [0:N_CHIPS-1];
    wire [N_CHIPS-1:0]         flash_ce_n;
    wire                        flash_oe_n;
    wire                        flash_we_n;

    // =====================================================================
    // DUT
    // =====================================================================

    nor_flash_raid #(
        .N_CHIPS(N_CHIPS),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .addr       (addr),
        .rw_n       (rw_n),
        .start      (start),
        .ready      (ready),
        .din        (din),
        .dout       (dout),
        .dout_valid (dout_valid),
        .flash_a    (flash_a),
        .flash_dq   (flash_dq),
        .flash_ce_n (flash_ce_n),
        .flash_oe_n (flash_oe_n),
        .flash_we_n (flash_we_n)
    );

    // =====================================================================
    // Flash model — simple async memory with tristate
    // =====================================================================

    genvar g;
    generate
        for (g = 0; g < N_CHIPS; g = g + 1) begin : flash_model
            reg [FLASH_DW-1:0] mem [0:(1<<FLASH_AW)-1];
            reg [FLASH_DW-1:0] dq_out;
            wire driving = !flash_ce_n[g] && !flash_oe_n && flash_we_n;

            // Read: async, with 90ns delay modeled as combinational for sim
            always @(*) begin
                if (!flash_ce_n[g])
                    dq_out = mem[flash_a];
                else
                    dq_out = {FLASH_DW{1'bx}};
            end

            assign flash_dq[g] = driving ? dq_out : {FLASH_DW{1'bz}};

            // Write: capture on WE# rising edge
            always @(posedge flash_we_n) begin
                if (!flash_ce_n[g]) begin
                    mem[flash_a] <= flash_dq[g];
                end
            end

            // Init memory with known pattern
            integer j;
            initial begin
                for (j = 0; j < (1<<FLASH_AW); j = j + 1)
                    mem[j] = (g << 12) | (j & 16'hFFF);  // chip_id in upper bits
            end
        end
    endgenerate

    // =====================================================================
    // Clock
    // =====================================================================

    initial clk = 0;
    always #16.667 clk = ~clk;   // ~30MHz

    // =====================================================================
    // Tests
    // =====================================================================

    integer pass_cnt, fail_cnt;
    reg [BLOCK_W-1:0] expected;

    task check_read;
        input [FLASH_AW-1:0] test_addr;
        input [BLOCK_W-1:0] exp;
        begin
            @(posedge clk);
            addr  <= test_addr;
            rw_n  <= 1'b1;
            start <= 1'b1;
            @(posedge clk);
            start <= 1'b0;

            // Wait for completion
            wait(dout_valid);
            @(posedge clk);

            if (dout === exp) begin
                $display("PASS: read addr=%0h got=%0h", test_addr, dout);
                pass_cnt = pass_cnt + 1;
            end else begin
                $display("FAIL: read addr=%0h expected=%0h got=%0h", test_addr, exp, dout);
                fail_cnt = fail_cnt + 1;
            end

            // Wait for ready
            wait(ready);
            @(posedge clk);
        end
    endtask

    // Build expected read data: each chip returns (chip_id << 12) | (addr & 0xFFF)
    function [BLOCK_W-1:0] build_expected;
        input [FLASH_AW-1:0] a;
        integer c;
        begin
            build_expected = {BLOCK_W{1'b0}};
            for (c = 0; c < N_CHIPS; c = c + 1)
                build_expected[c*FLASH_DW +: FLASH_DW] = (c << 12) | (a & 16'hFFF);
        end
    endfunction

    initial begin
        $dumpfile("tb_nor_flash_raid.vcd");
        $dumpvars(0, tb_nor_flash_raid);

        pass_cnt = 0;
        fail_cnt = 0;
        rst_n    = 0;
        addr     = 0;
        rw_n     = 1;
        start    = 0;
        din      = 0;

        // Reset
        repeat(5) @(posedge clk);
        rst_n = 1;
        repeat(3) @(posedge clk);

        $display("=== NOR Flash RAID-0 Testbench ===");
        $display("Chips: %0d, Addr width: %0d, Block: %0d bits", N_CHIPS, FLASH_AW, BLOCK_W);
        $display("");

        // --- Test 1: Read address 0x00 ---
        $display("--- Test: Read various addresses ---");
        check_read(8'h00, build_expected(8'h00));
        check_read(8'h01, build_expected(8'h01));
        check_read(8'h42, build_expected(8'h42));
        check_read(8'hFF, build_expected(8'hFF));

        // --- Test 2: Read burst ---
        $display("--- Test: Burst reads ---");
        begin : burst_test
            integer k;
            for (k = 0; k < 16; k = k + 1) begin
                check_read(k[FLASH_AW-1:0], build_expected(k[FLASH_AW-1:0]));
            end
        end

        // --- Test 3: Write then read back ---
        $display("--- Test: Write and readback ---");
        begin : write_test
            reg [BLOCK_W-1:0] wdata;
            // Write 0xDEAD to all chips at address 0x10
            wdata = {BLOCK_W{1'b0}};
            begin : build_wdata
                integer c;
                for (c = 0; c < N_CHIPS; c = c + 1)
                    wdata[c*FLASH_DW +: FLASH_DW] = 16'hDEAD + c;
            end

            @(posedge clk);
            addr  <= 8'h10;
            rw_n  <= 1'b0;
            din   <= wdata;
            start <= 1'b1;
            @(posedge clk);
            start <= 1'b0;
            wait(ready);
            repeat(3) @(posedge clk);

            // Read back
            check_read(8'h10, wdata);
        end

        // --- Summary ---
        $display("");
        $display("=== Results: %0d PASS, %0d FAIL ===", pass_cnt, fail_cnt);
        if (fail_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        #100;
        $finish;
    end

    // Timeout
    initial begin
        #500000;
        $display("TIMEOUT");
        $finish;
    end

endmodule
