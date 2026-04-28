// SATA Link Layer Testbench
// Loopback test: two sata_link instances (host + device) wired back-to-back.
// Tests: simple frame, CRC, multi-frame, large frame, HOLD flow control, error injection.

`timescale 1ns / 1ps

module tb_sata_link;

    reg clk, rst_n;

    // Clock: 150 MHz (6.67ns period)
    initial clk = 0;
    always #3.33 clk = ~clk;

    // =========================================================================
    // Primitives (same as sata_link)
    // =========================================================================
    localparam [31:0] PRIM_HOLD = {8'hB5, 8'h95, 8'h95, 8'hBC};
    localparam [31:0] PRIM_SYNC = {8'h95, 8'h95, 8'h95, 8'hBC};

    // =========================================================================
    // Host-side transport interface
    // =========================================================================
    reg  [31:0] h_tx_data;
    reg         h_tx_valid;
    reg         h_tx_last;
    reg         h_tx_start;
    wire        h_tx_ready;
    wire        h_tx_done;
    wire        h_tx_err;

    wire [31:0] h_rx_data;
    wire        h_rx_valid;
    wire        h_rx_last;
    wire        h_rx_sof;
    wire        h_rx_err;

    // =========================================================================
    // Device-side transport interface
    // =========================================================================
    reg  [31:0] d_tx_data;
    reg         d_tx_valid;
    reg         d_tx_last;
    reg         d_tx_start;
    wire        d_tx_ready;
    wire        d_tx_done;
    wire        d_tx_err;

    wire [31:0] d_rx_data;
    wire        d_rx_valid;
    wire        d_rx_last;
    wire        d_rx_sof;
    wire        d_rx_err;

    // =========================================================================
    // PHY loopback wires with injection points
    // =========================================================================
    wire [31:0] host_phy_tx_data;
    wire [3:0]  host_phy_tx_isk;
    wire [31:0] dev_phy_tx_data;
    wire [3:0]  dev_phy_tx_isk;

    // Error injection: corrupt one bit on host→device path
    reg         inject_error;
    wire [31:0] dev_phy_rx_data = inject_error ? (host_phy_tx_data ^ 32'h0000_0001) : host_phy_tx_data;
    wire [3:0]  dev_phy_rx_isk  = host_phy_tx_isk;

    // HOLD injection: replace device→host path with HOLD primitive
    reg         inject_hold;
    wire [31:0] host_phy_rx_data = inject_hold ? PRIM_HOLD : dev_phy_tx_data;
    wire [3:0]  host_phy_rx_isk  = inject_hold ? 4'b0001   : dev_phy_tx_isk;

    // =========================================================================
    // Instantiate host and device link layers
    // =========================================================================
    sata_link host_link (
        .clk          (clk),
        .rst_n        (rst_n),
        .phy_tx_data  (host_phy_tx_data),
        .phy_tx_isk   (host_phy_tx_isk),
        .phy_rx_data  (host_phy_rx_data),
        .phy_rx_isk   (host_phy_rx_isk),
        .phy_ready    (1'b1),
        .tx_data      (h_tx_data),
        .tx_valid     (h_tx_valid),
        .tx_last      (h_tx_last),
        .tx_ready     (h_tx_ready),
        .tx_start     (h_tx_start),
        .tx_done      (h_tx_done),
        .tx_err       (h_tx_err),
        .rx_data      (h_rx_data),
        .rx_valid     (h_rx_valid),
        .rx_last      (h_rx_last),
        .rx_sof       (h_rx_sof),
        .rx_err       (h_rx_err)
    );

    sata_link dev_link (
        .clk          (clk),
        .rst_n        (rst_n),
        .phy_tx_data  (dev_phy_tx_data),
        .phy_tx_isk   (dev_phy_tx_isk),
        .phy_rx_data  (dev_phy_rx_data),
        .phy_rx_isk   (dev_phy_rx_isk),
        .phy_ready    (1'b1),
        .tx_data      (d_tx_data),
        .tx_valid     (d_tx_valid),
        .tx_last      (d_tx_last),
        .tx_ready     (d_tx_ready),
        .tx_start     (d_tx_start),
        .tx_done      (d_tx_done),
        .tx_err       (d_tx_err),
        .rx_data      (d_rx_data),
        .rx_valid     (d_rx_valid),
        .rx_last      (d_rx_last),
        .rx_sof       (d_rx_sof),
        .rx_err       (d_rx_err)
    );

    // =========================================================================
    // RX capture buffer for device side
    // =========================================================================
    reg [31:0] rx_capture [0:1023];
    integer    rx_cap_idx;
    reg        rx_got_sof;
    reg        rx_got_last;
    reg        rx_got_err;

    // TX completion capture (pulses — must be latched)
    reg        tx_got_done;
    reg        tx_got_err;

    always @(posedge clk) begin
        if (d_rx_sof)   rx_got_sof  <= 1'b1;
        if (d_rx_valid) begin
            rx_capture[rx_cap_idx] <= d_rx_data;
            rx_cap_idx <= rx_cap_idx + 1;
        end
        if (d_rx_last)  rx_got_last <= 1'b1;
        if (d_rx_err)   rx_got_err  <= 1'b1;
        if (h_tx_done)  tx_got_done <= 1'b1;
        if (h_tx_err)   tx_got_err  <= 1'b1;
    end

    // =========================================================================
    // TX data source buffer
    // =========================================================================
    reg [31:0] tx_source [0:1023];
    integer    tx_src_idx;
    integer    tx_src_len;

    // =========================================================================
    // Test infrastructure
    // =========================================================================
    integer test_num;
    integer pass_count;
    integer fail_count;
    integer timeout_count;
    integer i;
    reg     test_pass;

    task reset_dut;
        begin
            rst_n = 0;
            h_tx_data  = 32'd0;
            h_tx_valid = 0;
            h_tx_last  = 0;
            h_tx_start = 0;
            d_tx_data  = 32'd0;
            d_tx_valid = 0;
            d_tx_last  = 0;
            d_tx_start = 0;
            inject_error = 0;
            inject_hold  = 0;
            rx_cap_idx  = 0;
            rx_got_sof  = 0;
            rx_got_last = 0;
            rx_got_err  = 0;
            tx_got_done = 0;
            tx_got_err  = 0;
            #20;
            rst_n = 1;
            #20;
        end
    endtask

    task clear_rx_capture;
        begin
            rx_cap_idx  = 0;
            rx_got_sof  = 0;
            rx_got_last = 0;
            rx_got_err  = 0;
            tx_got_done = 0;
            tx_got_err  = 0;
        end
    endtask

    // Send a frame from host: loads tx_source[0..len-1], drives the handshake
    task send_frame;
        input integer len;
        integer si;
        begin
            tx_src_len = len;
            tx_src_idx = 0;

            // Pulse tx_start for one cycle
            @(posedge clk);
            h_tx_start <= 1;
            @(posedge clk);
            h_tx_start <= 0;

            // Wait for tx_ready (entering TX_DATA)
            timeout_count = 0;
            while (!h_tx_ready && timeout_count < 200) begin
                @(posedge clk);
                timeout_count = timeout_count + 1;
            end

            if (timeout_count >= 200) begin
                $display("  ERROR: tx_ready timeout");
            end

            // Feed DWORDs
            for (si = 0; si < len; si = si + 1) begin
                h_tx_data  <= tx_source[si];
                h_tx_valid <= 1;
                h_tx_last  <= (si == len - 1) ? 1'b1 : 1'b0;
                @(posedge clk);
                // Wait if tx_ready deasserts (HOLD flow control)
                while (!h_tx_ready) begin
                    @(posedge clk);
                end
            end
            h_tx_valid <= 0;
            h_tx_last  <= 0;
            h_tx_data  <= 32'd0;
        end
    endtask

    // Wait for host tx_done or tx_err
    task wait_tx_complete;
        begin
            timeout_count = 0;
            while (!tx_got_done && !tx_got_err && timeout_count < 500) begin
                @(posedge clk);
                timeout_count = timeout_count + 1;
            end
        end
    endtask

    // Wait for device to finish receiving (rx_last or rx_err)
    task wait_rx_complete;
        begin
            timeout_count = 0;
            while (!rx_got_last && !rx_got_err && timeout_count < 500) begin
                @(posedge clk);
                timeout_count = timeout_count + 1;
            end
            // Allow a few more cycles for signals to settle
            repeat (5) @(posedge clk);
        end
    endtask

    // =========================================================================
    // Main test sequence
    // =========================================================================
    initial begin
        $dumpfile("tb_sata_link.vcd");
        $dumpvars(0, tb_sata_link);

        pass_count = 0;
        fail_count = 0;

        reset_dut;

        // =================================================================
        // Test 1: Simple frame TX/RX — 4 DWORDs
        // =================================================================
        test_num = 1;
        clear_rx_capture;
        tx_source[0] = 32'hDEAD_BEEF;
        tx_source[1] = 32'hCAFE_BABE;
        tx_source[2] = 32'h1234_5678;
        tx_source[3] = 32'h9ABC_DEF0;

        send_frame(4);
        wait_tx_complete;
        wait_rx_complete;

        test_pass = 1;
        if (!tx_got_done) begin test_pass = 0; $display("  T1: tx_done not asserted"); end
        if (tx_got_err)   begin test_pass = 0; $display("  T1: tx_err asserted"); end
        if (!rx_got_sof) begin test_pass = 0; $display("  T1: no SOF"); end
        if (!rx_got_last) begin test_pass = 0; $display("  T1: no rx_last"); end
        if (rx_got_err)  begin test_pass = 0; $display("  T1: rx_err asserted"); end
        if (rx_cap_idx != 4) begin test_pass = 0; $display("  T1: expected 4 DWORDs, got %0d", rx_cap_idx); end
        for (i = 0; i < 4 && i < rx_cap_idx; i = i + 1) begin
            if (rx_capture[i] !== tx_source[i]) begin
                test_pass = 0;
                $display("  T1: DWORD[%0d] mismatch: got %08h, exp %08h", i, rx_capture[i], tx_source[i]);
            end
        end
        if (test_pass) begin pass_count = pass_count + 1; $display("Test 1 (Simple frame TX/RX): PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 1 (Simple frame TX/RX): FAIL"); end

        // Let link settle
        repeat (20) @(posedge clk);

        // =================================================================
        // Test 2: CRC check — verify no error on valid frame
        // =================================================================
        test_num = 2;
        clear_rx_capture;
        tx_source[0] = 32'hAAAA_5555;
        tx_source[1] = 32'h0000_FFFF;

        send_frame(2);
        wait_tx_complete;
        wait_rx_complete;

        test_pass = 1;
        if (!tx_got_done)  begin test_pass = 0; $display("  T2: tx_done not asserted"); end
        if (rx_got_err)  begin test_pass = 0; $display("  T2: rx_err asserted (CRC fail)"); end
        if (rx_cap_idx != 2) begin test_pass = 0; $display("  T2: expected 2 DWORDs, got %0d", rx_cap_idx); end
        for (i = 0; i < 2 && i < rx_cap_idx; i = i + 1) begin
            if (rx_capture[i] !== tx_source[i]) begin
                test_pass = 0;
                $display("  T2: DWORD[%0d] mismatch: got %08h, exp %08h", i, rx_capture[i], tx_source[i]);
            end
        end
        if (test_pass) begin pass_count = pass_count + 1; $display("Test 2 (CRC check):          PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 2 (CRC check):          FAIL"); end

        repeat (20) @(posedge clk);

        // =================================================================
        // Test 3: Multi-frame — 3 frames back-to-back
        // =================================================================
        test_num = 3;
        test_pass = 1;

        begin : multi_frame_block
            integer frame_idx, flen, fi;
            reg [31:0] frame_data [0:2][0:7];
            integer frame_lens [0:2];

            frame_lens[0] = 3;
            frame_lens[1] = 5;
            frame_lens[2] = 2;

            // Fill frame data
            for (frame_idx = 0; frame_idx < 3; frame_idx = frame_idx + 1)
                for (fi = 0; fi < 8; fi = fi + 1)
                    frame_data[frame_idx][fi] = (frame_idx + 1) * 32'h1111_1111 + fi;

            for (frame_idx = 0; frame_idx < 3; frame_idx = frame_idx + 1) begin
                clear_rx_capture;
                flen = frame_lens[frame_idx];
                for (fi = 0; fi < flen; fi = fi + 1)
                    tx_source[fi] = frame_data[frame_idx][fi];

                send_frame(flen);
                wait_tx_complete;
                wait_rx_complete;

                if (!tx_got_done) begin test_pass = 0; $display("  T3: frame %0d tx_done fail", frame_idx); end
                if (rx_got_err) begin test_pass = 0; $display("  T3: frame %0d rx_err", frame_idx); end
                if (rx_cap_idx != flen) begin test_pass = 0; $display("  T3: frame %0d len mismatch: got %0d exp %0d", frame_idx, rx_cap_idx, flen); end
                for (fi = 0; fi < flen && fi < rx_cap_idx; fi = fi + 1) begin
                    if (rx_capture[fi] !== frame_data[frame_idx][fi]) begin
                        test_pass = 0;
                        $display("  T3: frame %0d DWORD[%0d] mismatch", frame_idx, fi);
                    end
                end

                repeat (20) @(posedge clk);
            end
        end

        if (test_pass) begin pass_count = pass_count + 1; $display("Test 3 (Multi-frame):        PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 3 (Multi-frame):        FAIL"); end

        repeat (20) @(posedge clk);

        // =================================================================
        // Test 4: Large frame — 128 DWORDs
        // =================================================================
        test_num = 4;
        clear_rx_capture;
        for (i = 0; i < 128; i = i + 1)
            tx_source[i] = 32'hA000_0000 + i;

        send_frame(128);
        wait_tx_complete;
        wait_rx_complete;

        test_pass = 1;
        if (!tx_got_done) begin test_pass = 0; $display("  T4: tx_done not asserted"); end
        if (rx_got_err) begin test_pass = 0; $display("  T4: rx_err asserted"); end
        if (rx_cap_idx != 128) begin test_pass = 0; $display("  T4: expected 128 DWORDs, got %0d", rx_cap_idx); end
        for (i = 0; i < 128 && i < rx_cap_idx; i = i + 1) begin
            if (rx_capture[i] !== (32'hA000_0000 + i)) begin
                test_pass = 0;
                $display("  T4: DWORD[%0d] mismatch: got %08h, exp %08h", i, rx_capture[i], 32'hA000_0000 + i);
            end
        end
        if (test_pass) begin pass_count = pass_count + 1; $display("Test 4 (Large frame 128DW):  PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 4 (Large frame 128DW):  FAIL"); end

        repeat (20) @(posedge clk);

        // =================================================================
        // Test 5: HOLD flow control — inject HOLD during data phase
        // =================================================================
        test_num = 5;
        clear_rx_capture;
        for (i = 0; i < 8; i = i + 1)
            tx_source[i] = 32'hF000_0000 + i;

        // Start frame transmission
        fork
            begin
                send_frame(8);
            end
            begin : hold_inject_block
                // Wait until host is sending data (a few cycles after tx_start)
                integer wi;
                wi = 0;
                while (!h_tx_ready && wi < 100) begin
                    @(posedge clk);
                    wi = wi + 1;
                end
                // Let 2 data DWORDs through, then inject HOLD for 10 cycles
                repeat (3) @(posedge clk);
                inject_hold = 1;
                repeat (10) @(posedge clk);
                inject_hold = 0;
            end
        join

        wait_tx_complete;
        wait_rx_complete;

        test_pass = 1;
        if (!tx_got_done) begin test_pass = 0; $display("  T5: tx_done not asserted"); end
        if (rx_got_err) begin test_pass = 0; $display("  T5: rx_err asserted"); end
        if (rx_cap_idx != 8) begin test_pass = 0; $display("  T5: expected 8 DWORDs, got %0d", rx_cap_idx); end
        for (i = 0; i < 8 && i < rx_cap_idx; i = i + 1) begin
            if (rx_capture[i] !== (32'hF000_0000 + i)) begin
                test_pass = 0;
                $display("  T5: DWORD[%0d] mismatch: got %08h, exp %08h", i, rx_capture[i], 32'hF000_0000 + i);
            end
        end
        if (test_pass) begin pass_count = pass_count + 1; $display("Test 5 (HOLD flow control):  PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 5 (HOLD flow control):  FAIL"); end

        repeat (20) @(posedge clk);

        // =================================================================
        // Test 6: Error injection — corrupt a DWORD, verify R_ERR
        // =================================================================
        test_num = 6;
        clear_rx_capture;
        tx_source[0] = 32'h1111_1111;
        tx_source[1] = 32'h2222_2222;
        tx_source[2] = 32'h3333_3333;
        tx_source[3] = 32'h4444_4444;

        fork
            begin
                send_frame(4);
            end
            begin : error_inject_block
                integer wi;
                wi = 0;
                // Wait for host to start sending data
                while (!h_tx_ready && wi < 100) begin
                    @(posedge clk);
                    wi = wi + 1;
                end
                // Let first 2 DWORDs through clean, then corrupt one
                repeat (2) @(posedge clk);
                inject_error = 1;
                @(posedge clk);
                inject_error = 0;
            end
        join

        wait_tx_complete;
        wait_rx_complete;

        test_pass = 1;
        // Host should get tx_err (R_ERR from device)
        if (!tx_got_err && !tx_got_done) begin
            test_pass = 0;
            $display("  T6: neither tx_err nor tx_done asserted (timeout?)");
        end
        // Device should report rx_err
        if (!rx_got_err) begin test_pass = 0; $display("  T6: rx_err not asserted (expected CRC error)"); end

        if (test_pass) begin pass_count = pass_count + 1; $display("Test 6 (Error injection):    PASS"); end
        else           begin fail_count = fail_count + 1; $display("Test 6 (Error injection):    FAIL"); end

        // =================================================================
        // Summary
        // =================================================================
        $display("");
        $display("========================================");
        $display("  Results: %0d passed, %0d failed", pass_count, fail_count);
        $display("========================================");
        if (fail_count == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        #100;
        $finish;
    end

    // Global timeout
    initial begin
        #500000;
        $display("GLOBAL TIMEOUT");
        $finish;
    end

endmodule
