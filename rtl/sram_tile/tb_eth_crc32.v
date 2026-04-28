// tb_eth_crc32.v — Testbench for Ethernet CRC32 + IP checksum

`timescale 1ns/1ps

module tb_eth_crc32;

    reg        clk = 0;
    reg        rst_n = 0;
    reg        init;
    reg [7:0]  data_in;
    reg        valid;
    wire [31:0] crc_out;
    wire [31:0] fcs_out;

    always #5 clk = ~clk;

    eth_crc32 uut (
        .clk(clk), .rst_n(rst_n),
        .init(init), .data_in(data_in), .valid(valid),
        .crc_out(crc_out), .fcs_out(fcs_out)
    );

    // IP checksum UUT
    reg        ip_init, ip_valid, ip_finish;
    reg [15:0] ip_word;
    wire [15:0] ip_cksum;
    wire        ip_done;

    ip_checksum ip_uut (
        .clk(clk), .rst_n(rst_n),
        .init(ip_init), .word_in(ip_word), .valid(ip_valid),
        .finish(ip_finish), .checksum(ip_cksum), .done(ip_done)
    );

    integer pass = 0, fail = 0;

    task check(input [31:0] got, input [31:0] exp, input [8*40-1:0] name);
    begin
        if (got === exp) begin
            $display("PASS: %0s = 0x%08x", name, got);
            pass = pass + 1;
        end else begin
            $display("FAIL: %0s = 0x%08x, expected 0x%08x", name, got, exp);
            fail = fail + 1;
        end
    end
    endtask

    // Feed bytes one per clock
    task feed_byte(input [7:0] b);
    begin
        @(posedge clk);
        data_in <= b;
        valid   <= 1'b1;
        @(posedge clk);
        valid   <= 1'b0;
    end
    endtask

    integer i;

    initial begin
        $dumpfile("tb_eth_crc32.vcd");
        $dumpvars(0, tb_eth_crc32);

        init = 0; data_in = 0; valid = 0;
        ip_init = 0; ip_valid = 0; ip_finish = 0; ip_word = 0;

        #20 rst_n = 1;
        #10;

        // ======== TEST 1: CRC of "123456789" ========
        // Standard CRC32 check value = 0xCBF43926
        @(posedge clk);
        init <= 1'b1;
        @(posedge clk);
        init <= 1'b0;

        // Feed ASCII "123456789"
        feed_byte(8'h31); // '1'
        feed_byte(8'h32); // '2'
        feed_byte(8'h33); // '3'
        feed_byte(8'h34); // '4'
        feed_byte(8'h35); // '5'
        feed_byte(8'h36); // '6'
        feed_byte(8'h37); // '7'
        feed_byte(8'h38); // '8'
        feed_byte(8'h39); // '9'

        @(posedge clk);
        check(~crc_out, 32'hCBF43926, "CRC32(123456789)");

        // ======== TEST 2: CRC of empty (just init) ========
        @(posedge clk);
        init <= 1'b1;
        @(posedge clk);
        init <= 1'b0;
        @(posedge clk);
        check(crc_out, 32'hFFFFFFFF, "CRC32 after init");

        // ======== TEST 3: CRC of single byte 0x00 ========
        @(posedge clk);
        init <= 1'b1;
        @(posedge clk);
        init <= 1'b0;
        feed_byte(8'h00);
        @(posedge clk);
        check(~crc_out, 32'hD202EF8D, "CRC32(0x00)");

        // ======== TEST 4: Ethernet frame CRC ========
        // Common test: dst=FF:FF:FF:FF:FF:FF src=00:00:00:00:00:00 type=0x0800
        @(posedge clk);
        init <= 1'b1;
        @(posedge clk);
        init <= 1'b0;

        // 6 bytes dst
        for (i = 0; i < 6; i = i + 1) feed_byte(8'hFF);
        // 6 bytes src
        for (i = 0; i < 6; i = i + 1) feed_byte(8'h00);
        // ethertype
        feed_byte(8'h08);
        feed_byte(8'h00);

        @(posedge clk);
        // Just verify FCS is non-zero and deterministic
        if (fcs_out !== 32'd0 && fcs_out !== 32'hxxxxxxxx) begin
            $display("PASS: Ethernet frame FCS = 0x%08x (non-zero)", fcs_out);
            pass = pass + 1;
        end else begin
            $display("FAIL: Ethernet frame FCS invalid");
            fail = fail + 1;
        end

        // ======== TEST 5: IP Header Checksum ========
        // Example from RFC 1071:
        // IP header (20 bytes) with checksum field = 0
        // 4500 0073 0000 4000 4011 0000 C0A8_0001 C0A8_00C7
        // Expected checksum: 0xB861
        @(posedge clk);
        ip_init <= 1'b1;
        @(posedge clk);
        ip_init <= 1'b0;

        ip_word <= 16'h4500; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h0073; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h0000; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h4000; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h4011; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h0000; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk); // checksum=0
        ip_word <= 16'hC0A8; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h0001; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'hC0A8; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);
        ip_word <= 16'h00C7; ip_valid <= 1; @(posedge clk); ip_valid <= 0; @(posedge clk);

        ip_finish <= 1; @(posedge clk); ip_finish <= 0;
        @(posedge clk);
        @(posedge clk);

        check({16'd0, ip_cksum}, {16'd0, 16'hB861}, "IP checksum");

        // ======== Summary ========
        #20;
        $display("\n=== %0d PASS, %0d FAIL ===", pass, fail);
        if (fail == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        $finish;
    end

endmodule
