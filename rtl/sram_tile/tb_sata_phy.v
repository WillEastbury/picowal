// Testbench for sata_phy — OOB initialization sequence
// Mock SATA device responds to host OOB handshake.
//
// Tests:
//   1. Normal OOB → READY
//   2. Timeout/retry on missed COMINIT
//   3. Link-layer data passthrough in READY
//   4. Loss-of-alignment recovery

`timescale 1ns / 1ps

module tb_sata_phy;

    // Use short OOB timings for fast simulation
    localparam OOB_SHORT       = 1;
    localparam BURST_LEN       = 4;
    localparam COMRESET_GAP    = 4;
    localparam COMWAKE_GAP     = 4;
    localparam OOB_BURST_COUNT = 6;
    localparam COMINIT_TIMEOUT = 64;
    localparam COMWAKE_TIMEOUT = 64;
    localparam ALIGN_TIMEOUT   = 64;
    localparam RETRY_LIMIT     = 3;

    localparam [31:0] PRIM_ALIGN = 32'h7B4A_4ABC;
    localparam [3:0]  ALIGN_ISK  = 4'b0001;

    reg         clk;
    reg         rst_n;

    // SERDES stub signals
    wire [31:0] tx_data;
    wire [3:0]  tx_charisk;
    wire        tx_comreset;
    wire        tx_comwake;
    reg  [31:0] rx_data;
    reg  [3:0]  rx_charisk;
    reg         rx_cominit;
    reg         rx_comwake;
    reg         rx_byte_aligned;

    // Link-layer interface
    wire        phy_ready;
    wire [1:0]  phy_speed;
    reg  [31:0] link_tx_data;
    reg  [3:0]  link_tx_isk;
    reg         link_tx_valid;
    wire [31:0] link_rx_data;
    wire [3:0]  link_rx_isk;
    wire        link_rx_valid;

    // Test bookkeeping
    integer test_num;
    integer pass_count;
    integer fail_count;

    // DUT
    sata_phy #(
        .OOB_SHORT       (OOB_SHORT),
        .BURST_LEN       (BURST_LEN),
        .COMRESET_GAP    (COMRESET_GAP),
        .COMWAKE_GAP     (COMWAKE_GAP),
        .OOB_BURST_COUNT (OOB_BURST_COUNT),
        .COMINIT_TIMEOUT (COMINIT_TIMEOUT),
        .COMWAKE_TIMEOUT (COMWAKE_TIMEOUT),
        .ALIGN_TIMEOUT   (ALIGN_TIMEOUT),
        .RETRY_LIMIT     (RETRY_LIMIT)
    ) dut (
        .clk             (clk),
        .rst_n           (rst_n),
        .tx_data         (tx_data),
        .tx_charisk      (tx_charisk),
        .tx_comreset     (tx_comreset),
        .tx_comwake      (tx_comwake),
        .rx_data         (rx_data),
        .rx_charisk      (rx_charisk),
        .rx_cominit      (rx_cominit),
        .rx_comwake      (rx_comwake),
        .rx_byte_aligned (rx_byte_aligned),
        .phy_ready       (phy_ready),
        .phy_speed       (phy_speed),
        .link_tx_data    (link_tx_data),
        .link_tx_isk     (link_tx_isk),
        .link_tx_valid   (link_tx_valid),
        .link_rx_data    (link_rx_data),
        .link_rx_isk     (link_rx_isk),
        .link_rx_valid   (link_rx_valid)
    );

    // ----------------------------------------------------------------
    // Clock: 150 MHz → 6.667 ns period (use 6 ns for simplicity)
    // ----------------------------------------------------------------
    initial clk = 0;
    always #3 clk = ~clk;

    // ----------------------------------------------------------------
    // Helper tasks
    // ----------------------------------------------------------------
    task reset_dut;
        begin
            rst_n          <= 0;
            rx_data        <= 32'd0;
            rx_charisk     <= 4'd0;
            rx_cominit     <= 0;
            rx_comwake     <= 0;
            rx_byte_aligned <= 0;
            link_tx_data   <= 32'd0;
            link_tx_isk    <= 4'd0;
            link_tx_valid  <= 0;
            repeat (4) @(posedge clk);
            rst_n <= 1;
            @(posedge clk);
        end
    endtask

    // Wait until tx_comreset has been seen pulsing (COMRESET phase done)
    // We detect the transition: tx_comreset was 1 at some point, then
    // the DUT moves past ST_RESET (burst_cnt reaches OOB_BURST_COUNT).
    task wait_comreset_done;
        integer seen;
        begin
            seen = 0;
            // Wait for at least one tx_comreset assertion
            while (!seen) begin
                @(posedge clk);
                if (tx_comreset) seen = 1;
            end
            // Now wait until COMRESET bursts finish (tx_comreset stays low for a while)
            // The DUT enters ST_WAIT_COMINIT after all bursts
            // Simple: wait for tx_comreset to go low and stay low for GAP+2 cycles
            begin : wait_low_block
                integer low_count;
                low_count = 0;
                while (low_count < (COMRESET_GAP + BURST_LEN + 2)) begin
                    @(posedge clk);
                    if (!tx_comreset)
                        low_count = low_count + 1;
                    else
                        low_count = 0;
                end
            end
        end
    endtask

    task wait_comwake_done;
        integer seen;
        begin
            seen = 0;
            while (!seen) begin
                @(posedge clk);
                if (tx_comwake) seen = 1;
            end
            begin : wait_low_block2
                integer low_count;
                low_count = 0;
                while (low_count < (COMWAKE_GAP + BURST_LEN + 2)) begin
                    @(posedge clk);
                    if (!tx_comwake)
                        low_count = low_count + 1;
                    else
                        low_count = 0;
                end
            end
        end
    endtask

    task report(input integer tnum, input integer cond);
        begin
            if (cond) begin
                $display("TEST %0d: PASS", tnum);
                pass_count = pass_count + 1;
            end else begin
                $display("TEST %0d: FAIL", tnum);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // ----------------------------------------------------------------
    // Perform a full OOB handshake (device side)
    // Returns 1 if phy_ready asserted, 0 if timeout
    // ----------------------------------------------------------------
    task do_full_handshake(output integer ok);
        begin
            ok = 0;

            // 1) Wait for COMRESET to finish
            wait_comreset_done;

            // 2) Reply with COMINIT
            @(posedge clk);
            rx_cominit <= 1;
            @(posedge clk);
            rx_cominit <= 0;

            // 3) Wait for COMWAKE from host
            wait_comwake_done;

            // 4) Reply with COMWAKE
            @(posedge clk);
            rx_comwake <= 1;
            @(posedge clk);
            rx_comwake <= 0;

            // 5) Wait a few clocks, then send ALIGN
            repeat (3) @(posedge clk);
            rx_data         <= PRIM_ALIGN;
            rx_charisk      <= ALIGN_ISK;
            rx_byte_aligned <= 1;

            // Wait for phy_ready or timeout
            begin : handshake_wait
                integer i;
                for (i = 0; i < ALIGN_TIMEOUT + 10; i = i + 1) begin
                    @(posedge clk);
                    if (phy_ready) begin
                        ok = 1;
                        disable handshake_wait;
                    end
                end
            end
        end
    endtask

    // ----------------------------------------------------------------
    // Main test sequence
    // ----------------------------------------------------------------
    initial begin
        $dumpfile("sata_phy.vcd");
        $dumpvars(0, tb_sata_phy);

        pass_count = 0;
        fail_count = 0;

        // ==============================================================
        // TEST 1: Normal OOB handshake → PHY reaches READY
        // ==============================================================
        test_num = 1;
        $display("\n--- TEST 1: Normal OOB handshake ---");
        reset_dut;

        begin : test1_block
            integer ok;
            do_full_handshake(ok);
            report(1, ok && phy_ready && (phy_speed == 2'd1));
        end

        // ==============================================================
        // TEST 2: Timeout / retry on missed COMINIT
        //   First COMRESET cycle: don't reply → should retry
        //   Second COMRESET cycle: reply normally → READY
        // ==============================================================
        test_num = 2;
        $display("\n--- TEST 2: COMINIT timeout then retry ---");
        reset_dut;

        begin : test2_block
            integer ok;

            // First COMRESET: let it complete, do NOT send COMINIT → timeout
            wait_comreset_done;
            // Wait for the COMINIT timeout to expire and DUT to re-enter ST_RESET
            repeat (COMINIT_TIMEOUT + 20) @(posedge clk);

            // Second COMRESET: reply normally
            do_full_handshake(ok);
            report(2, ok && phy_ready);
        end

        // ==============================================================
        // TEST 3: Data passthrough in READY state
        // ==============================================================
        test_num = 3;
        $display("\n--- TEST 3: Link-layer data passthrough ---");
        // PHY should still be ready from test 2 or we redo handshake
        if (!phy_ready) begin
            reset_dut;
            begin : test3_hs
                integer ok;
                do_full_handshake(ok);
            end
        end

        begin : test3_block
            reg [31:0] test_word;
            reg [3:0]  test_isk;
            reg        tx_ok, rx_ok;

            // TX: link layer sends data, should appear on tx_data
            test_word = 32'hDEAD_BEEF;
            test_isk  = 4'b0000;
            link_tx_data  <= test_word;
            link_tx_isk   <= test_isk;
            link_tx_valid <= 1;
            @(posedge clk);
            @(posedge clk); // one pipeline delay
            tx_ok = (tx_data == test_word) && (tx_charisk == test_isk);
            link_tx_valid <= 0;

            // RX: device sends data, should appear on link_rx_data
            rx_data         <= 32'hCAFE_BABE;
            rx_charisk      <= 4'b0010;
            rx_byte_aligned <= 1;
            @(posedge clk);
            rx_ok = (link_rx_data == 32'hCAFE_BABE) &&
                    (link_rx_isk  == 4'b0010) &&
                    link_rx_valid;

            report(3, tx_ok && rx_ok);
        end

        // ==============================================================
        // TEST 4: Loss-of-alignment recovery
        // ==============================================================
        test_num = 4;
        $display("\n--- TEST 4: Loss-of-alignment recovery ---");

        // Ensure we start in READY
        if (!phy_ready) begin
            reset_dut;
            begin : test4_hs
                integer ok;
                do_full_handshake(ok);
            end
        end

        begin : test4_block
            integer ok;

            // Drop alignment
            rx_byte_aligned <= 0;
            rx_data         <= 32'd0;
            rx_charisk      <= 4'd0;
            repeat (4) @(posedge clk);

            // PHY should drop phy_ready
            if (phy_ready) begin
                // Give it one more cycle
                @(posedge clk);
            end

            report(4, !phy_ready && (phy_speed == 2'd0));

            // Now do a full re-handshake to confirm recovery
            // (DUT should have returned to ST_RESET)
            do_full_handshake(ok);
            report(5, ok && phy_ready && (phy_speed == 2'd1));
        end

        // ==============================================================
        // Summary
        // ==============================================================
        $display("\n========================================");
        $display("  %0d / %0d tests PASSED", pass_count, pass_count + fail_count);
        if (fail_count == 0)
            $display("  ALL TESTS PASSED");
        else
            $display("  %0d TESTS FAILED", fail_count);
        $display("========================================\n");

        $finish;
    end

    // Global timeout watchdog
    initial begin
        #500000;
        $display("TIMEOUT: simulation exceeded 500 us");
        $finish;
    end

endmodule
