// tb_udp_page_tx.v — Testbench for UDP page fragmenter

`timescale 1ns/1ps

module tb_udp_page_tx;

    reg         clk = 0;
    reg         rst_n = 0;
    always #5 clk = ~clk;

    // Page buffer (4KB, filled with pattern)
    reg [7:0]  page_buf [0:4095];
    wire [11:0] buf_addr;
    wire [7:0]  buf_data = page_buf[buf_addr];

    reg         start;
    reg [31:0]  req_tag;
    reg [7:0]   resp_flags;
    wire        done, busy;
    wire [7:0]  tx_data;
    wire        tx_valid, tx_sof, tx_eof;
    reg         tx_ready;

    udp_page_tx uut (
        .clk(clk), .rst_n(rst_n),
        .buf_addr(buf_addr), .buf_data(buf_data),
        .start(start), .req_tag(req_tag), .resp_flags(resp_flags),
        .done(done), .busy(busy),
        .tx_data(tx_data), .tx_valid(tx_valid),
        .tx_sof(tx_sof), .tx_eof(tx_eof),
        .tx_ready(tx_ready)
    );

    integer i;
    integer pass = 0, fail = 0;
    integer total_bytes;
    integer frag_count;
    integer frag_bytes [0:2];

    task check(input integer got, input integer exp, input [8*40-1:0] name);
    begin
        if (got == exp) begin
            $display("PASS: %0s = %0d", name, got);
            pass = pass + 1;
        end else begin
            $display("FAIL: %0s = %0d, expected %0d", name, got, exp);
            fail = fail + 1;
        end
    end
    endtask

    initial begin
        $dumpfile("tb_udp_page_tx.vcd");
        $dumpvars(0, tb_udp_page_tx);

        start = 0; req_tag = 32'hDEADBEEF; resp_flags = 8'h00;
        tx_ready = 1;

        // Fill page with ascending pattern
        for (i = 0; i < 4096; i = i + 1)
            page_buf[i] = i[7:0];

        #20 rst_n = 1;
        #20;

        // ======== TEST: Fragment a 4KB page ========
        @(posedge clk);
        start <= 1'b1;
        @(posedge clk);
        start <= 1'b0;

        total_bytes = 0;
        frag_count = 0;
        frag_bytes[0] = 0;
        frag_bytes[1] = 0;
        frag_bytes[2] = 0;

        // Count bytes per fragment
        while (!done) begin
            @(posedge clk);
            if (tx_valid) begin
                total_bytes = total_bytes + 1;
                if (frag_count < 3)
                    frag_bytes[frag_count] = frag_bytes[frag_count] + 1;
            end
            if (tx_eof && tx_valid) begin
                frag_count = frag_count + 1;
            end
        end

        @(posedge clk);

        check(frag_count, 3, "fragment count");
        check(frag_bytes[0], 1408, "frag 0 bytes (hdr+data)");
        check(frag_bytes[1], 1408, "frag 1 bytes (hdr+data)");
        check(frag_bytes[2], 1304, "frag 2 bytes (hdr+data)");
        check(total_bytes, 4096 + 24, "total bytes (4096 data + 3×8 hdr)");

        // ======== TEST 2: Backpressure ========
        // Set tx_ready=0 intermittently
        @(posedge clk);
        start <= 1'b1;
        @(posedge clk);
        start <= 1'b0;

        total_bytes = 0;
        frag_count = 0;

        while (!done) begin
            @(posedge clk);
            tx_ready <= ($random % 3 != 0);  // ~67% ready
            if (tx_valid) total_bytes = total_bytes + 1;
            if (tx_eof && tx_valid) frag_count = frag_count + 1;
        end
        tx_ready <= 1;

        @(posedge clk);
        check(frag_count, 3, "backpressure frag count");
        check(total_bytes, 4096 + 24, "backpressure total bytes");

        // ======== Summary ========
        #20;
        $display("\n=== %0d PASS, %0d FAIL ===", pass, fail);
        if (fail == 0)
            $display("ALL TESTS PASSED");

        $finish;
    end

endmodule
