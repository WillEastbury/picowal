// Testbench for SATA KV Engine
`timescale 1ns / 1ps

module tb_sata_kv_engine;

    reg         clk;
    reg         rst_n;

    // Network ingress
    reg         cmd_valid;
    reg  [7:0]  cmd_flags;
    reg  [63:0] cmd_addr;
    wire        cmd_ready;

    reg  [31:0] net_wr_data;
    reg         net_wr_valid;
    reg         net_wr_last;
    wire        net_wr_ready;

    // SATA command interface
    wire        sata_cmd_valid;
    reg         sata_cmd_ready;
    wire        sata_cmd_is_write;
    wire [47:0] sata_cmd_lba;
    wire [15:0] sata_cmd_count;

    wire [31:0] sata_wr_data;
    wire        sata_wr_valid;
    wire        sata_wr_last;
    reg         sata_wr_ready;

    reg  [31:0] sata_rd_data;
    reg         sata_rd_valid;
    reg         sata_rd_last;

    reg         sata_cmd_complete;
    reg         sata_cmd_error;
    reg  [7:0]  sata_cmd_status;

    // Response
    wire [31:0] resp_data;
    wire        resp_valid;
    wire        resp_last;
    reg         resp_ready;

    wire        resp_start;
    wire [31:0] resp_tag;
    wire [7:0]  resp_flags;

    wire        ack_valid;
    wire [7:0]  ack_byte;

    // Stats
    wire [31:0] reads_completed;
    wire [31:0] writes_completed;
    wire [31:0] error_count;

    integer pass_count;
    integer fail_count;

    sata_kv_engine dut (
        .clk              (clk),
        .rst_n            (rst_n),
        .cmd_valid        (cmd_valid),
        .cmd_flags        (cmd_flags),
        .cmd_addr         (cmd_addr),
        .cmd_ready        (cmd_ready),
        .net_wr_data      (net_wr_data),
        .net_wr_valid     (net_wr_valid),
        .net_wr_last      (net_wr_last),
        .net_wr_ready     (net_wr_ready),
        .sata_cmd_valid   (sata_cmd_valid),
        .sata_cmd_ready   (sata_cmd_ready),
        .sata_cmd_is_write(sata_cmd_is_write),
        .sata_cmd_lba     (sata_cmd_lba),
        .sata_cmd_count   (sata_cmd_count),
        .sata_wr_data     (sata_wr_data),
        .sata_wr_valid    (sata_wr_valid),
        .sata_wr_last     (sata_wr_last),
        .sata_wr_ready    (sata_wr_ready),
        .sata_rd_data     (sata_rd_data),
        .sata_rd_valid    (sata_rd_valid),
        .sata_rd_last     (sata_rd_last),
        .sata_cmd_complete(sata_cmd_complete),
        .sata_cmd_error   (sata_cmd_error),
        .sata_cmd_status  (sata_cmd_status),
        .resp_data        (resp_data),
        .resp_valid       (resp_valid),
        .resp_last        (resp_last),
        .resp_ready       (resp_ready),
        .resp_start       (resp_start),
        .resp_tag         (resp_tag),
        .resp_flags       (resp_flags),
        .ack_valid        (ack_valid),
        .ack_byte         (ack_byte),
        .reads_completed  (reads_completed),
        .writes_completed (writes_completed),
        .errors           (error_count)
    );

    // Clock: 50 MHz = 20ns period
    initial clk = 0;
    always #10 clk = ~clk;

    // =====================================================================
    // Helper tasks
    // =====================================================================

    task reset;
        begin
            rst_n          <= 1'b0;
            cmd_valid      <= 1'b0;
            cmd_flags      <= 8'd0;
            cmd_addr       <= 64'd0;
            net_wr_data    <= 32'd0;
            net_wr_valid   <= 1'b0;
            net_wr_last    <= 1'b0;
            sata_cmd_ready <= 1'b1;
            sata_wr_ready  <= 1'b1;
            sata_rd_data   <= 32'd0;
            sata_rd_valid  <= 1'b0;
            sata_rd_last   <= 1'b0;
            sata_cmd_complete <= 1'b0;
            sata_cmd_error <= 1'b0;
            sata_cmd_status<= 8'd0;
            resp_ready     <= 1'b1;
            repeat (5) @(posedge clk);
            rst_n          <= 1'b1;
            repeat (2) @(posedge clk);
        end
    endtask

    task inject_cmd;
        input [7:0]  flags;
        input [63:0] addr;
        begin
            @(posedge clk);
            cmd_valid <= 1'b1;
            cmd_flags <= flags;
            cmd_addr  <= addr;
            @(posedge clk);
            while (!cmd_ready) @(posedge clk);
            @(posedge clk);
            cmd_valid <= 1'b0;
        end
    endtask

    // Wait for sata_cmd_valid handshake
    task wait_sata_cmd;
        begin
            while (!sata_cmd_valid) @(posedge clk);
            // Let the handshake complete
            @(posedge clk);
        end
    endtask

    // SATA mock: stream read data (1024 DWORDs)
    task sata_mock_read_data;
        input [31:0] lba_low;
        integer i;
        begin
            repeat (2) @(posedge clk);
            for (i = 0; i < 1024; i = i + 1) begin
                sata_rd_data  <= (i[31:0] ^ lba_low);
                sata_rd_valid <= 1'b1;
                sata_rd_last  <= (i == 1023) ? 1'b1 : 1'b0;
                @(posedge clk);
            end
            sata_rd_valid <= 1'b0;
            sata_rd_last  <= 1'b0;
            repeat (1) @(posedge clk);
            sata_cmd_complete <= 1'b1;
            @(posedge clk);
            sata_cmd_complete <= 1'b0;
        end
    endtask

    // SATA mock: accept write data (1024 DWORDs) and store for checking
    reg [31:0] wr_capture [0:1023];
    integer    wr_capture_cnt;

    task sata_mock_accept_write;
        begin
            wr_capture_cnt = 0;
            while (wr_capture_cnt < 1024) begin
                @(posedge clk);
                if (sata_wr_valid && sata_wr_ready) begin
                    wr_capture[wr_capture_cnt] = sata_wr_data;
                    wr_capture_cnt = wr_capture_cnt + 1;
                end
            end
            repeat (2) @(posedge clk);
            sata_cmd_complete <= 1'b1;
            @(posedge clk);
            sata_cmd_complete <= 1'b0;
        end
    endtask

    // Collect response data
    reg [31:0] resp_capture [0:1023];
    integer    resp_capture_cnt;

    task collect_response;
        begin
            resp_capture_cnt = 0;
            while (1) begin
                @(posedge clk);
                if (resp_valid && resp_ready) begin
                    resp_capture[resp_capture_cnt] = resp_data;
                    resp_capture_cnt = resp_capture_cnt + 1;
                    if (resp_last) begin
                        disable collect_response;
                    end
                end
            end
        end
    endtask

    // Stream net write data (1024 DWORDs)
    task stream_net_write;
        input [31:0] pattern_base;
        integer i;
        begin
            for (i = 0; i < 1024; i = i + 1) begin
                net_wr_data  <= (i[31:0] ^ pattern_base);
                net_wr_valid <= 1'b1;
                net_wr_last  <= (i == 1023) ? 1'b1 : 1'b0;
                @(posedge clk);
                while (!net_wr_ready) @(posedge clk);
            end
            net_wr_valid <= 1'b0;
            net_wr_last  <= 1'b0;
        end
    endtask

    // =====================================================================
    // Tests
    // =====================================================================

    integer i;
    integer errs;
    reg [47:0] expected_lba;

    initial begin
        $dumpfile("sata_kv_engine.vcd");
        $dumpvars(0, tb_sata_kv_engine);

        pass_count = 0;
        fail_count = 0;

        // =============================================
        // Test 1: READ
        // =============================================
        reset;
        $display("--- Test 1: READ ---");

        // Inject read command: flags=0x00, addr=0x100
        fork
            inject_cmd(8'h00, 64'h0000_0000_0000_0100);
        join

        // Wait for SATA command
        wait_sata_cmd;

        // Verify LBA = 0x100 * 8 = 0x800
        expected_lba = {3'b000, 42'h100, 3'b000};
        if (sata_cmd_lba !== expected_lba) begin
            $display("  FAIL: LBA mismatch: got %h, expected %h", sata_cmd_lba, expected_lba);
            fail_count = fail_count + 1;
        end else if (sata_cmd_is_write !== 1'b0) begin
            $display("  FAIL: is_write should be 0 for READ");
            fail_count = fail_count + 1;
        end else if (sata_cmd_count !== 16'd8) begin
            $display("  FAIL: sector count should be 8, got %d", sata_cmd_count);
            fail_count = fail_count + 1;
        end else begin
            $display("  SATA cmd OK: LBA=%h, count=%d, is_write=%b", sata_cmd_lba, sata_cmd_count, sata_cmd_is_write);
        end

        // Mock SATA read data and collect response in parallel
        fork
            sata_mock_read_data(32'h0000_0800);
            collect_response;
        join

        // Verify response data
        errs = 0;
        for (i = 0; i < resp_capture_cnt && i < 1024; i = i + 1) begin
            if (resp_capture[i] !== (i[31:0] ^ 32'h0000_0800)) begin
                if (errs < 5)
                    $display("  FAIL: resp[%0d]=%h, expected=%h", i, resp_capture[i], (i[31:0] ^ 32'h0000_0800));
                errs = errs + 1;
            end
        end

        if (resp_capture_cnt != 1024) begin
            $display("  FAIL: received %0d DWORDs, expected 1024", resp_capture_cnt);
            fail_count = fail_count + 1;
        end else if (errs > 0) begin
            $display("  FAIL: %0d data mismatches", errs);
            fail_count = fail_count + 1;
        end else begin
            $display("  PASS: READ test - 1024 DWORDs correct");
            pass_count = pass_count + 1;
        end

        // Wait for engine to return to IDLE
        repeat (10) @(posedge clk);

        // =============================================
        // Test 2: WRITE
        // =============================================
        $display("--- Test 2: WRITE ---");

        // Inject write command and stream data
        fork
            inject_cmd(8'h01, 64'h0000_0000_0000_0200);
        join

        // Stream network write data
        fork
            stream_net_write(32'hCAFE_0000);
        join

        // Wait for SATA command
        wait_sata_cmd;

        expected_lba = {3'b000, 42'h200, 3'b000};
        if (sata_cmd_lba !== expected_lba) begin
            $display("  FAIL: LBA mismatch: got %h, expected %h", sata_cmd_lba, expected_lba);
            fail_count = fail_count + 1;
        end else if (sata_cmd_is_write !== 1'b1) begin
            $display("  FAIL: is_write should be 1 for WRITE");
            fail_count = fail_count + 1;
        end else begin
            $display("  SATA cmd OK: LBA=%h, is_write=%b", sata_cmd_lba, sata_cmd_is_write);
        end

        // Accept write data from engine
        sata_mock_accept_write;

        // Wait for ACK
        while (!ack_valid) @(posedge clk);

        // Verify write data
        errs = 0;
        for (i = 0; i < 1024; i = i + 1) begin
            if (wr_capture[i] !== (i[31:0] ^ 32'hCAFE_0000)) begin
                if (errs < 5)
                    $display("  FAIL: wr[%0d]=%h, expected=%h", i, wr_capture[i], (i[31:0] ^ 32'hCAFE_0000));
                errs = errs + 1;
            end
        end

        if (errs > 0) begin
            $display("  FAIL: %0d write data mismatches", errs);
            fail_count = fail_count + 1;
        end else if (ack_byte !== 8'h00) begin
            $display("  FAIL: ack_byte=%h, expected 0x00", ack_byte);
            fail_count = fail_count + 1;
        end else begin
            $display("  PASS: WRITE test - 1024 DWORDs + ACK correct");
            pass_count = pass_count + 1;
        end

        repeat (10) @(posedge clk);

        // =============================================
        // Test 3: Back-to-back (READ then WRITE)
        // =============================================
        $display("--- Test 3: Back-to-back ---");

        // READ
        fork
            inject_cmd(8'h00, 64'h0000_0000_0000_0300);
        join

        wait_sata_cmd;

        fork
            sata_mock_read_data(32'h0000_1800);
            collect_response;
        join

        errs = 0;
        for (i = 0; i < resp_capture_cnt && i < 1024; i = i + 1) begin
            if (resp_capture[i] !== (i[31:0] ^ 32'h0000_1800))
                errs = errs + 1;
        end

        if (resp_capture_cnt != 1024 || errs > 0) begin
            $display("  FAIL: B2B READ got %0d words, %0d errs", resp_capture_cnt, errs);
            fail_count = fail_count + 1;
        end else begin
            $display("  B2B READ OK");
        end

        repeat (10) @(posedge clk);

        // WRITE
        fork
            inject_cmd(8'h01, 64'h0000_0000_0000_0400);
        join

        fork
            stream_net_write(32'hBEEF_0000);
        join

        wait_sata_cmd;
        sata_mock_accept_write;

        while (!ack_valid) @(posedge clk);

        errs = 0;
        for (i = 0; i < 1024; i = i + 1) begin
            if (wr_capture[i] !== (i[31:0] ^ 32'hBEEF_0000))
                errs = errs + 1;
        end

        if (errs > 0 || ack_byte !== 8'h00) begin
            $display("  FAIL: B2B WRITE %0d errs, ack=%h", errs, ack_byte);
            fail_count = fail_count + 1;
        end else begin
            $display("  B2B WRITE OK");
            $display("  PASS: Back-to-back test");
            pass_count = pass_count + 1;
        end

        repeat (10) @(posedge clk);

        // =============================================
        // Test 4: Error handling
        // =============================================
        $display("--- Test 4: Error handling ---");

        // READ with error
        fork
            inject_cmd(8'h00, 64'h0000_0000_0000_0500);
        join

        wait_sata_cmd;

        // Inject error instead of data
        repeat (3) @(posedge clk);
        sata_cmd_error <= 1'b1;
        @(posedge clk);
        sata_cmd_error <= 1'b0;

        // Collect error response
        collect_response;

        if (resp_flags !== 8'hFF) begin
            $display("  FAIL: error resp_flags=%h, expected 0xFF", resp_flags);
            fail_count = fail_count + 1;
        end else begin
            $display("  Error READ resp_flags=0xFF OK");
        end

        repeat (10) @(posedge clk);

        // WRITE with error
        fork
            inject_cmd(8'h01, 64'h0000_0000_0000_0600);
        join

        fork
            stream_net_write(32'hDEAD_0000);
        join

        wait_sata_cmd;

        // Inject error during write
        repeat (3) @(posedge clk);
        sata_cmd_error <= 1'b1;
        @(posedge clk);
        sata_cmd_error <= 1'b0;

        // Wait for ACK with error
        while (!ack_valid) @(posedge clk);

        if (ack_byte !== 8'hFF) begin
            $display("  FAIL: error ack_byte=%h, expected 0xFF", ack_byte);
            fail_count = fail_count + 1;
        end else begin
            $display("  Error WRITE ack_byte=0xFF OK");
            $display("  PASS: Error handling test");
            pass_count = pass_count + 1;
        end

        repeat (10) @(posedge clk);

        // =============================================
        // Summary
        // =============================================
        $display("");
        $display("============================");
        $display("  %0d PASSED, %0d FAILED", pass_count, fail_count);
        if (fail_count == 0)
            $display("  ALL TESTS PASSED");
        else
            $display("  SOME TESTS FAILED");
        $display("============================");
        $display("  Stats: reads=%0d writes=%0d errors=%0d",
                 reads_completed, writes_completed, error_count);

        $finish;
    end

    // Watchdog
    initial begin
        #5000000;
        $display("WATCHDOG TIMEOUT");
        $finish;
    end

endmodule
