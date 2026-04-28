// Testbench for SATA Transport Layer
`timescale 1ns / 1ps

module tb_sata_transport;

    reg         clk;
    reg         rst_n;

    // Link TX (output from DUT)
    wire [31:0] link_tx_data;
    wire        link_tx_valid;
    wire        link_tx_last;
    reg         link_tx_ready;
    wire        link_tx_start;
    reg         link_tx_done;
    reg         link_tx_err;

    // Link RX (input to DUT)
    reg  [31:0] link_rx_data;
    reg         link_rx_valid;
    reg         link_rx_last;
    reg         link_rx_sof;
    reg         link_rx_err;

    // Command TX
    reg         cmd_tx_start;
    reg  [7:0]  cmd_tx_command;
    reg  [47:0] cmd_tx_lba;
    reg  [15:0] cmd_tx_count;
    reg  [7:0]  cmd_tx_features;
    reg  [7:0]  cmd_tx_device;
    wire        cmd_tx_done;
    wire        cmd_tx_err_out;

    // Data TX
    reg         data_tx_start;
    reg  [31:0] data_tx_dword;
    reg         data_tx_valid;
    reg         data_tx_last;
    wire        data_tx_ready;
    wire        data_tx_done;

    // RX outputs
    wire        rx_reg_fis_valid;
    wire [7:0]  rx_status;
    wire [7:0]  rx_error;
    wire        rx_pio_setup_valid;
    wire [15:0] rx_pio_xfer_count;
    wire [7:0]  rx_pio_status;
    wire        rx_dma_activate;
    wire [31:0] rx_data_dword;
    wire        rx_data_valid;
    wire        rx_data_last;
    wire        rx_data_err;

    sata_transport dut (
        .clk              (clk),
        .rst_n            (rst_n),
        .link_tx_data     (link_tx_data),
        .link_tx_valid    (link_tx_valid),
        .link_tx_last     (link_tx_last),
        .link_tx_ready    (link_tx_ready),
        .link_tx_start    (link_tx_start),
        .link_tx_done     (link_tx_done),
        .link_tx_err      (link_tx_err),
        .link_rx_data     (link_rx_data),
        .link_rx_valid    (link_rx_valid),
        .link_rx_last     (link_rx_last),
        .link_rx_sof      (link_rx_sof),
        .link_rx_err      (link_rx_err),
        .cmd_tx_start     (cmd_tx_start),
        .cmd_tx_command   (cmd_tx_command),
        .cmd_tx_lba       (cmd_tx_lba),
        .cmd_tx_count     (cmd_tx_count),
        .cmd_tx_features  (cmd_tx_features),
        .cmd_tx_device    (cmd_tx_device),
        .cmd_tx_done      (cmd_tx_done),
        .cmd_tx_err       (cmd_tx_err_out),
        .data_tx_start    (data_tx_start),
        .data_tx_dword    (data_tx_dword),
        .data_tx_valid    (data_tx_valid),
        .data_tx_last     (data_tx_last),
        .data_tx_ready    (data_tx_ready),
        .data_tx_done     (data_tx_done),
        .rx_reg_fis_valid (rx_reg_fis_valid),
        .rx_status        (rx_status),
        .rx_error         (rx_error),
        .rx_pio_setup_valid(rx_pio_setup_valid),
        .rx_pio_xfer_count(rx_pio_xfer_count),
        .rx_pio_status    (rx_pio_status),
        .rx_dma_activate  (rx_dma_activate),
        .rx_data_dword    (rx_data_dword),
        .rx_data_valid    (rx_data_valid),
        .rx_data_last     (rx_data_last),
        .rx_data_err      (rx_data_err)
    );

    // Clock: 10ns period
    initial clk = 0;
    always #5 clk = ~clk;

    // Captured TX DWORDs
    reg [31:0] captured_dw [0:31];
    integer    cap_idx;
    reg        cap_last_seen;

    // Capture link TX output
    always @(posedge clk) begin
        if (link_tx_valid) begin
            captured_dw[cap_idx] <= link_tx_data;
            cap_idx <= cap_idx + 1;
            if (link_tx_last)
                cap_last_seen <= 1;
        end
    end

    integer pass_count;
    integer fail_count;

    task reset_dut;
        begin
            rst_n = 0;
            link_tx_ready = 0;
            link_tx_done  = 0;
            link_tx_err   = 0;
            link_rx_data  = 0;
            link_rx_valid = 0;
            link_rx_last  = 0;
            link_rx_sof   = 0;
            link_rx_err   = 0;
            cmd_tx_start  = 0;
            cmd_tx_command = 0;
            cmd_tx_lba    = 0;
            cmd_tx_count  = 0;
            cmd_tx_features = 0;
            cmd_tx_device = 0;
            data_tx_start = 0;
            data_tx_dword = 0;
            data_tx_valid = 0;
            data_tx_last  = 0;
            cap_idx       = 0;
            cap_last_seen = 0;
            #20;
            rst_n = 1;
            #10;
        end
    endtask

    task send_cmd_fis(
        input [7:0]  command,
        input [47:0] lba,
        input [15:0] count,
        input [7:0]  features,
        input [7:0]  device
    );
        begin
            @(posedge clk);
            cmd_tx_start   <= 1;
            cmd_tx_command <= command;
            cmd_tx_lba     <= lba;
            cmd_tx_count   <= count;
            cmd_tx_features <= features;
            cmd_tx_device  <= device;
            cap_idx        <= 0;
            cap_last_seen  <= 0;
            @(posedge clk);
            cmd_tx_start <= 0;
            // Wait for link_tx_start pulse
            @(posedge clk);
            // Now provide link_tx_ready
            link_tx_ready <= 1;
            // Wait until all 5 DWORDs sent (cap_last_seen)
            repeat (20) @(posedge clk);
            link_tx_ready <= 0;
            // Signal link_tx_done
            link_tx_done <= 1;
            @(posedge clk);
            link_tx_done <= 0;
            // Wait for cmd_tx_done
            repeat (3) @(posedge clk);
        end
    endtask

    // Inject an RX FIS: array of DWORDs
    task inject_rx_fis;
        input integer num_dw;
        input [31:0] dw0, dw1, dw2, dw3, dw4;
        integer i;
        reg [31:0] dws [0:4];
        begin
            dws[0] = dw0; dws[1] = dw1; dws[2] = dw2; dws[3] = dw3; dws[4] = dw4;
            for (i = 0; i < num_dw; i = i + 1) begin
                @(posedge clk);
                link_rx_data  <= dws[i];
                link_rx_valid <= 1;
                link_rx_sof   <= (i == 0) ? 1 : 0;
                link_rx_last  <= (i == num_dw - 1) ? 1 : 0;
            end
            @(posedge clk);
            link_rx_valid <= 0;
            link_rx_sof   <= 0;
            link_rx_last  <= 0;
            link_rx_data  <= 0;
        end
    endtask

    initial begin
        pass_count = 0;
        fail_count = 0;

        // =====================================================================
        // Test 1: H2D Register FIS — READ DMA EXT (cmd=0x25)
        // LBA=0x123456789ABC, count=8
        // =====================================================================
        reset_dut;
        $display("\n--- Test 1: H2D Register FIS (READ DMA EXT) ---");
        send_cmd_fis(8'h25, 48'h123456789ABC, 16'd8, 8'h00, 8'hE0);

        // Verify 5 DWORDs
        // DW0: [7:0]=0x27, [15]=C=1(bit15), [23:16]=cmd=0x25, [31:24]=features=0x00
        // Expected: 0x00_25_80_27
        if (captured_dw[0] !== 32'h00258027) begin
            $display("  FAIL DW0: got %08h, expected 00258027", captured_dw[0]);
            fail_count = fail_count + 1;
        end else begin
            $display("  DW0 OK: %08h", captured_dw[0]);
        end

        // DW1: [7:0]=LBA_low=0x9A(lba[7:0])=BC, [15:8]=LBA_mid=0x78(lba[15:8]), [23:16]=LBA_high=0x56(lba[23:16])
        //       [27:24]=device[3:0]=0(E0&0F=0), [31:28]=lba[27:24]
        // lba = 48'h1234_5678_9ABC
        // lba[7:0]=BC, lba[15:8]=9A, lba[23:16]=78, device=E0
        // [27:24]=device[3:0]=0, [31:28]=lba[27:24]=5
        // Expected: 0x5_0_78_9A_BC = {4'h5, 4'h0, 8'h78, 8'h9A, 8'hBC}
        // Wait, device is E0 so device[3:0] = 0. lba[27:24]=5.
        // DW1 = {lba[27:24], device[3:0], lba[23:16], lba[15:8], lba[7:0]}
        //      = {4'h5, 4'h0, 8'h78, 8'h9A, 8'hBC} = 32'h50789ABC
        if (captured_dw[1] !== 32'h50789ABC) begin
            $display("  FAIL DW1: got %08h, expected 50789ABC", captured_dw[1]);
            fail_count = fail_count + 1;
        end else begin
            $display("  DW1 OK: %08h", captured_dw[1]);
        end

        // DW2: [7:0]=LBA_low_exp=lba[31:24]=56, [15:8]=LBA_mid_exp=lba[39:32]=34,
        //       [23:16]=LBA_high_exp=lba[47:40]=12, [31:24]=features_exp=0
        // Expected: 0x00_12_34_56
        if (captured_dw[2] !== 32'h00123456) begin
            $display("  FAIL DW2: got %08h, expected 00123456", captured_dw[2]);
            fail_count = fail_count + 1;
        end else begin
            $display("  DW2 OK: %08h", captured_dw[2]);
        end

        // DW3: [15:0]=sector_count=8, rest 0
        // Expected: 0x00000008
        if (captured_dw[3] !== 32'h00000008) begin
            $display("  FAIL DW3: got %08h, expected 00000008", captured_dw[3]);
            fail_count = fail_count + 1;
        end else begin
            $display("  DW3 OK: %08h", captured_dw[3]);
        end

        // DW4: reserved = 0
        if (captured_dw[4] !== 32'h00000000) begin
            $display("  FAIL DW4: got %08h, expected 00000000", captured_dw[4]);
            fail_count = fail_count + 1;
        end else begin
            $display("  DW4 OK: %08h", captured_dw[4]);
        end

        if (cap_last_seen !== 1) begin
            $display("  FAIL: tx_last not asserted");
            fail_count = fail_count + 1;
        end

        if (cmd_tx_done !== 0) begin
            // cmd_tx_done is a pulse, should be gone by now, but let's check it fired
        end

        // Check all 5 passed
        if (captured_dw[0] === 32'h00258027 && captured_dw[1] === 32'h50789ABC &&
            captured_dw[2] === 32'h00123456 && captured_dw[3] === 32'h00000008 &&
            captured_dw[4] === 32'h00000000 && cap_last_seen) begin
            $display("  Test 1: PASS");
            pass_count = pass_count + 1;
        end else begin
            $display("  Test 1: FAIL");
        end

        // =====================================================================
        // Test 2: H2D Register FIS — IDENTIFY DEVICE (cmd=0xEC)
        // LBA=0, count=0, features=0, device=0xE0
        // =====================================================================
        reset_dut;
        $display("\n--- Test 2: H2D Register FIS (IDENTIFY DEVICE) ---");
        send_cmd_fis(8'hEC, 48'h000000000000, 16'd0, 8'h00, 8'hE0);

        // DW0: 0x00_EC_80_27
        // DW1: {lba[27:24]=0, device[3:0]=0, lba[23:16]=0, lba[15:8]=0, lba[7:0]=0} = 0
        // DW2: 0
        // DW3: 0
        // DW4: 0
        begin : test2_block
            reg t2_pass;
            t2_pass = 1;
            if (captured_dw[0] !== 32'h00EC8027) begin
                $display("  FAIL DW0: got %08h, expected 00EC8027", captured_dw[0]);
                t2_pass = 0;
            end
            if (captured_dw[1] !== 32'h00000000) begin
                $display("  FAIL DW1: got %08h, expected 00000000", captured_dw[1]);
                t2_pass = 0;
            end
            if (captured_dw[2] !== 32'h00000000) begin
                $display("  FAIL DW2: got %08h, expected 00000000", captured_dw[2]);
                t2_pass = 0;
            end
            if (captured_dw[3] !== 32'h00000000) begin
                $display("  FAIL DW3: got %08h, expected 00000000", captured_dw[3]);
                t2_pass = 0;
            end
            if (captured_dw[4] !== 32'h00000000) begin
                $display("  FAIL DW4: got %08h, expected 00000000", captured_dw[4]);
                t2_pass = 0;
            end
            if (!cap_last_seen) begin
                $display("  FAIL: tx_last not asserted");
                t2_pass = 0;
            end
            if (t2_pass) begin
                $display("  Test 2: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 2: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Test 3: Data FIS TX — 16 DWORDs of data
        // =====================================================================
        reset_dut;
        $display("\n--- Test 3: Data FIS TX (16 DWORDs) ---");
        begin : test3_block
            integer i;
            reg t3_pass;
            t3_pass = 1;

            @(posedge clk);
            data_tx_start <= 1;
            cap_idx       <= 0;
            cap_last_seen <= 0;
            @(posedge clk);
            data_tx_start <= 0;
            // Wait a cycle for FSM to see start, then assert ready
            @(posedge clk);
            link_tx_ready <= 1;
            // Wait for header to be sent
            repeat (2) @(posedge clk);

            // Now stream 16 data DWORDs
            for (i = 0; i < 16; i = i + 1) begin
                data_tx_dword <= (i + 1) * 32'h11111111;
                data_tx_valid <= 1;
                data_tx_last  <= (i == 15) ? 1 : 0;
                @(posedge clk);
            end
            data_tx_valid <= 0;
            data_tx_last  <= 0;

            repeat (5) @(posedge clk);
            link_tx_ready <= 0;

            // Signal done from link
            link_tx_done <= 1;
            @(posedge clk);
            link_tx_done <= 0;
            repeat (3) @(posedge clk);

            // Verify: first DWORD should be 0x46 header
            if (captured_dw[0] !== {24'd0, 8'h46}) begin
                $display("  FAIL DW0 (header): got %08h, expected 00000046", captured_dw[0]);
                t3_pass = 0;
            end
            // Verify payload DWORDs
            for (i = 0; i < 16; i = i + 1) begin
                if (captured_dw[i+1] !== (i + 1) * 32'h11111111) begin
                    $display("  FAIL DW%0d: got %08h, expected %08h", i+1, captured_dw[i+1], (i+1)*32'h11111111);
                    t3_pass = 0;
                end
            end
            if (!cap_last_seen) begin
                $display("  FAIL: tx_last not asserted");
                t3_pass = 0;
            end
            if (t3_pass) begin
                $display("  Test 3: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 3: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Test 4: D2H Register FIS RX (type=0x34)
        // DW0: type=0x34, interrupt=1(bit14), status=0x50, error=0x00
        // =====================================================================
        reset_dut;
        $display("\n--- Test 4: D2H Register FIS RX ---");
        begin : test4_block
            reg t4_pass;
            t4_pass = 1;

            // DW0: {error, status, 1'b0(res), 1'b1(I), ...zeros, type}
            // = {8'h00, 8'h50, 1'b0, 1'b1, 6'b0, 8'h34}
            // = 32'h00_50_40_34
            inject_rx_fis(5,
                32'h00504034,  // DW0
                32'h00000000,  // DW1
                32'h00000000,  // DW2
                32'h00000000,  // DW3
                32'h00000000   // DW4
            );
            repeat (5) @(posedge clk);

            if (!rx_reg_fis_valid && rx_status !== 8'h50) begin
                // Check on outputs (they're registered, may need a cycle)
            end
            // rx_reg_fis_valid is a pulse; check latched status/error
            if (rx_status !== 8'h50) begin
                $display("  FAIL status: got %02h, expected 50", rx_status);
                t4_pass = 0;
            end
            if (rx_error !== 8'h00) begin
                $display("  FAIL error: got %02h, expected 00", rx_error);
                t4_pass = 0;
            end
            if (t4_pass) begin
                $display("  Test 4: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 4: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Test 5: PIO Setup FIS RX (type=0x5F)
        // DW0: type=0x5F, status=0x58, error=0x00
        // DW4: transfer_count=512 (0x0200)
        // =====================================================================
        reset_dut;
        $display("\n--- Test 5: PIO Setup FIS RX ---");
        begin : test5_block
            reg t5_pass;
            t5_pass = 1;

            inject_rx_fis(5,
                32'h0058005F,  // DW0: {error=00, status=58, flags=00, type=5F}
                32'h00000000,  // DW1
                32'h00000000,  // DW2
                32'h00000000,  // DW3
                32'h00000200   // DW4: xfer_count=0x0200=512
            );
            repeat (5) @(posedge clk);

            if (rx_pio_xfer_count !== 16'h0200) begin
                $display("  FAIL xfer_count: got %04h, expected 0200", rx_pio_xfer_count);
                t5_pass = 0;
            end
            if (rx_pio_status !== 8'h58) begin
                $display("  FAIL pio_status: got %02h, expected 58", rx_pio_status);
                t5_pass = 0;
            end
            if (t5_pass) begin
                $display("  Test 5: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 5: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Test 6: DMA Activate FIS RX (type=0x39, 1 DWORD)
        // =====================================================================
        reset_dut;
        $display("\n--- Test 6: DMA Activate FIS RX ---");
        begin : test6_block
            reg t6_pass;
            reg saw_activate;
            t6_pass = 1;
            saw_activate = 0;

            // Inject single DWORD
            @(posedge clk);
            link_rx_data  <= 32'h00000039;
            link_rx_valid <= 1;
            link_rx_sof   <= 1;
            link_rx_last  <= 1;
            @(posedge clk);
            link_rx_valid <= 0;
            link_rx_sof   <= 0;
            link_rx_last  <= 0;

            repeat (5) begin
                @(posedge clk);
                if (rx_dma_activate) saw_activate = 1;
            end

            if (!saw_activate) begin
                $display("  FAIL: dma_activate not asserted");
                t6_pass = 0;
            end
            if (t6_pass) begin
                $display("  Test 6: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 6: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Test 7: Data FIS RX — header + 8 data DWORDs
        // =====================================================================
        reset_dut;
        $display("\n--- Test 7: Data FIS RX ---");
        begin : test7_block
            integer i;
            reg t7_pass;
            reg [31:0] rx_captured [0:15];
            integer rx_cap_idx;
            reg saw_last;
            t7_pass = 1;
            rx_cap_idx = 0;
            saw_last = 0;

            // Inject header DWORD (type=0x46) + 8 data DWORDs = 9 total
            @(posedge clk);
            link_rx_data  <= 32'h00000046;
            link_rx_valid <= 1;
            link_rx_sof   <= 1;
            link_rx_last  <= 0;
            @(posedge clk);
            link_rx_sof <= 0;

            for (i = 1; i <= 8; i = i + 1) begin
                link_rx_data <= i * 32'hAAAAAAAA;
                link_rx_last <= (i == 8) ? 1 : 0;
                @(posedge clk);
                // Capture rx_data output (from previous cycle's input)
                if (rx_data_valid) begin
                    rx_captured[rx_cap_idx] = rx_data_dword;
                    rx_cap_idx = rx_cap_idx + 1;
                    if (rx_data_last) saw_last = 1;
                end
            end
            link_rx_valid <= 0;
            link_rx_last  <= 0;

            // Capture remaining output
            repeat (5) begin
                @(posedge clk);
                if (rx_data_valid) begin
                    rx_captured[rx_cap_idx] = rx_data_dword;
                    rx_cap_idx = rx_cap_idx + 1;
                    if (rx_data_last) saw_last = 1;
                end
            end

            if (rx_cap_idx !== 8) begin
                $display("  FAIL: captured %0d data DWORDs, expected 8", rx_cap_idx);
                t7_pass = 0;
            end
            for (i = 0; i < 8 && i < rx_cap_idx; i = i + 1) begin
                if (rx_captured[i] !== (i + 1) * 32'hAAAAAAAA) begin
                    $display("  FAIL data[%0d]: got %08h, expected %08h",
                             i, rx_captured[i], (i+1)*32'hAAAAAAAA);
                    t7_pass = 0;
                end
            end
            if (!saw_last) begin
                $display("  FAIL: rx_data_last not asserted");
                t7_pass = 0;
            end
            if (t7_pass) begin
                $display("  Test 7: PASS");
                pass_count = pass_count + 1;
            end else begin
                $display("  Test 7: FAIL");
                fail_count = fail_count + 1;
            end
        end

        // =====================================================================
        // Summary
        // =====================================================================
        $display("\n==============================");
        $display("  Results: %0d PASS, %0d FAIL", pass_count, fail_count);
        $display("==============================\n");

        if (fail_count == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        $finish;
    end

endmodule
