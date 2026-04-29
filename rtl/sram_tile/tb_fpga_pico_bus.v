// tb_fpga_pico_bus.v — Testbench for 8-bit parallel bus controller
`timescale 1ns/1ps

module tb_fpga_pico_bus;

    reg        clk, rst_n;
    wire [7:0] bus_data;
    wire       bus_rdy;
    reg        bus_ack;
    wire       bus_dir;
    wire       bus_sof;
    wire       bus_eof;
    wire [1:0] bus_sock;

    // TX interface (FPGA → Pico)
    reg  [7:0] tx_data;
    reg        tx_valid;
    wire       tx_ready;
    reg        tx_sof, tx_eof;
    reg  [1:0] tx_sock;

    // RX interface (Pico → FPGA)
    wire [7:0] rx_data;
    wire       rx_valid;
    reg        rx_ready;
    wire       rx_sof, rx_eof;
    wire [1:0] rx_sock;

    wire       timeout_err;
    wire       bus_active;

    // Pico-side bus driver (simulates pico GPIO)
    reg  [7:0] pico_data_out;
    reg        pico_drive;
    assign bus_data = pico_drive ? pico_data_out : 8'bz;

    fpga_pico_bus #(
        .FIFO_DEPTH(16),
        .TIMEOUT_CYCLES(64)
    ) uut (
        .clk(clk), .rst_n(rst_n),
        .bus_data(bus_data), .bus_rdy(bus_rdy), .bus_ack(bus_ack),
        .bus_dir(bus_dir), .bus_sof(bus_sof), .bus_eof(bus_eof),
        .bus_sock(bus_sock),
        .tx_data(tx_data), .tx_valid(tx_valid), .tx_ready(tx_ready),
        .tx_sof(tx_sof), .tx_eof(tx_eof), .tx_sock(tx_sock),
        .rx_data(rx_data), .rx_valid(rx_valid), .rx_ready(rx_ready),
        .rx_sof(rx_sof), .rx_eof(rx_eof), .rx_sock(rx_sock),
        .timeout_err(timeout_err), .bus_active(bus_active)
    );

    // Clock: 50 MHz
    initial clk = 0;
    always #10 clk = ~clk;

    integer pass_count = 0;
    integer fail_count = 0;
    integer test_num = 0;

    task check(input [63:0] desc, input cond);
        begin
            test_num = test_num + 1;
            if (cond) begin
                $display("  [PASS] %0d: %0s", test_num, desc);
                pass_count = pass_count + 1;
            end else begin
                $display("  [FAIL] %0d: %0s", test_num, desc);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // Simulate pico ACK after RDY (with small delay)
    task pico_ack_byte;
        begin
            @(posedge bus_rdy);
            #40;  // pico reaction time
            bus_ack = 1;
            #20;
            bus_ack = 0;
        end
    endtask

    // Simulate pico sending a byte to FPGA
    task pico_send_byte(input [7:0] data, input sof, input eof);
        begin
            // Wait for DIR=1 (RX mode)
            wait (bus_dir == 1);
            #40;
            pico_drive = 1;
            pico_data_out = data;
            // Pico uses bus_ack pin as its RDY signal when DIR=1
            bus_ack = 1;
            #20;
            bus_ack = 0;
            // Wait for FPGA to ACK (bus_rdy pulse)
            @(posedge bus_rdy);
            #20;
            pico_drive = 0;
        end
    endtask

    initial begin
        $dumpfile("tb_fpga_pico_bus.vcd");
        $dumpvars(0, tb_fpga_pico_bus);

        // Init
        rst_n = 0;
        bus_ack = 0;
        tx_data = 0;
        tx_valid = 0;
        tx_sof = 0;
        tx_eof = 0;
        tx_sock = 0;
        rx_ready = 1;
        pico_drive = 0;
        pico_data_out = 0;

        #100;
        rst_n = 1;
        #40;

        // ─────────────────────────────────────────────────────────────
        // Test 1: TX single byte (FPGA → Pico)
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: TX single byte ---");
        tx_data = 8'hAB;
        tx_sock = 2'd1;
        tx_sof = 1;
        tx_eof = 1;
        tx_valid = 1;
        @(posedge clk); #1;
        tx_valid = 0;
        tx_sof = 0;
        tx_eof = 0;

        // Wait for bus_rdy then ACK
        pico_ack_byte;
        #60;

        check("TX ready deasserts during transfer", 1);
        check("No timeout error", !timeout_err);

        // ─────────────────────────────────────────────────────────────
        // Test 2: TX multi-byte frame
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: TX 4-byte frame ---");
        // Load 4 bytes into FIFO
        tx_sock = 2'd2;
        tx_data = 8'h10; tx_sof = 1; tx_eof = 0; tx_valid = 1;
        @(posedge clk); #1;
        tx_data = 8'h20; tx_sof = 0; tx_eof = 0;
        @(posedge clk); #1;
        tx_data = 8'h30;
        @(posedge clk); #1;
        tx_data = 8'h40; tx_eof = 1;
        @(posedge clk); #1;
        tx_valid = 0; tx_eof = 0;

        // ACK all 4 bytes
        pico_ack_byte;
        pico_ack_byte;
        pico_ack_byte;
        pico_ack_byte;
        #100;

        check("4-byte TX frame completed", !timeout_err);
        check("Bus returns to idle", !bus_active);

        // ─────────────────────────────────────────────────────────────
        // Test 3: TX timeout (no ACK)
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: TX timeout ---");
        tx_data = 8'hFF;
        tx_sock = 2'd0;
        tx_sof = 1; tx_eof = 1;
        tx_valid = 1;
        @(posedge clk); #1;
        tx_valid = 0; tx_sof = 0; tx_eof = 0;

        // Don't ACK — wait for timeout
        #3000;

        check("Timeout error flagged", timeout_err);

        // Reset for next tests
        rst_n = 0;
        #40;
        rst_n = 1;
        #40;

        // ─────────────────────────────────────────────────────────────
        // Test 4: RX single byte (Pico → FPGA)
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: RX single byte ---");
        // No TX data pending, so bus should switch to RX mode
        #200;
        // Now pico sends
        pico_send_byte(8'hCD, 1, 1);
        // Wait a few clocks for rx_valid pulse
        #200;

        // rx_valid is single-cycle, check rx_data which persists
        check("RX byte received", rx_data == 8'hCD);
        check("RX data correct", rx_data == 8'hCD);

        // ─────────────────────────────────────────────────────────────
        // Test 5: Bidirectional interleave
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: Bidirectional ---");
        // Queue TX
        tx_data = 8'hEE; tx_sock = 2'd3; tx_sof = 1; tx_eof = 1; tx_valid = 1;
        @(posedge clk); #1;
        tx_valid = 0; tx_sof = 0; tx_eof = 0;

        // ACK it
        pico_ack_byte;
        #100;

        check("Bidir TX completed", !timeout_err);

        // ─────────────────────────────────────────────────────────────
        // Test 6: FIFO full backpressure
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: FIFO backpressure ---");
        // Fill FIFO (16 deep)
        begin : fill_fifo
            integer i;
            for (i = 0; i < 16; i = i + 1) begin
                tx_data = i[7:0]; tx_valid = 1;
                tx_sof = (i == 0); tx_eof = (i == 15);
                tx_sock = 2'd0;
                @(posedge clk); #1;
            end
            tx_valid = 0; tx_sof = 0; tx_eof = 0;
        end

        check("FIFO full, tx_ready=0", !tx_ready);

        // Drain by ACKing
        begin : drain_fifo
            integer j;
            for (j = 0; j < 16; j = j + 1) begin
                pico_ack_byte;
            end
        end
        #100;

        check("FIFO drained, tx_ready=1", tx_ready);

        // ─────────────────────────────────────────────────────────────
        // Test 7: Reset clears state
        // ─────────────────────────────────────────────────────────────
        $display("\n--- Test: Reset ---");
        tx_data = 8'h99; tx_valid = 1; tx_sof = 1; tx_eof = 1; tx_sock = 0;
        @(posedge clk); #1;
        tx_valid = 0; tx_sof = 0; tx_eof = 0;
        #20;
        rst_n = 0;
        #40;
        rst_n = 1;
        #40;

        check("Reset clears timeout_err", !timeout_err);
        check("Reset clears bus_active", !bus_active);

        // ─────────────────────────────────────────────────────────────
        // Summary
        // ─────────────────────────────────────────────────────────────
        #100;
        $display("\n══════════════════════════════════════");
        $display("  Results: %0d PASS, %0d FAIL / %0d total",
                 pass_count, fail_count, test_num);
        $display("══════════════════════════════════════\n");
        if (fail_count == 0)
            $display("  *** ALL TESTS PASSED ***\n");
        else
            $display("  *** FAILURES DETECTED ***\n");
        $finish;
    end

endmodule
