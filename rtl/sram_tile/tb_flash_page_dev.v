// tb_flash_page_dev.v — Testbench for 512-byte page flash device

`timescale 1ns / 1ps

module tb_flash_page_dev;

    parameter N_CHIPS       = 4;
    parameter FLASH_AW      = 10;       // small for sim: 1K words
    parameter FLASH_DW      = 16;
    parameter PAGE_ADDR_W   = 4;        // 16 pages
    parameter BUS_W         = N_CHIPS * FLASH_DW;  // 64 bits
    parameter WORD_OFFSET_W = FLASH_AW - PAGE_ADDR_W;  // 6
    parameter WORDS_PER_PAGE = (1 << WORD_OFFSET_W);    // 64
    parameter PAGE_BYTES    = WORDS_PER_PAGE * (BUS_W / 8);  // 512

    reg                         clk;
    reg                         rst_n;
    reg  [PAGE_ADDR_W-1:0]     page_addr;
    reg                         rw_n;
    reg                         start;
    wire                        ready;
    wire [BUS_W-1:0]           dout;
    wire                        dout_valid;
    wire                        page_done;
    reg  [BUS_W-1:0]           din;
    reg                         din_valid;

    wire [FLASH_AW-1:0]        flash_a;
    wire [BUS_W-1:0]           flash_dq;
    wire [N_CHIPS-1:0]         flash_ce_n;
    wire                        flash_oe_n;
    wire                        flash_we_n;

    // =====================================================================
    // DUT
    // =====================================================================

    flash_page_dev #(
        .N_CHIPS(N_CHIPS),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW),
        .PAGE_ADDR_W(PAGE_ADDR_W)
    ) dut (
        .clk        (clk),
        .rst_n      (rst_n),
        .page_addr  (page_addr),
        .rw_n       (rw_n),
        .start      (start),
        .ready      (ready),
        .dout       (dout),
        .dout_valid (dout_valid),
        .page_done  (page_done),
        .din        (din),
        .din_valid  (din_valid),
        .flash_a    (flash_a),
        .flash_dq   (flash_dq),
        .flash_ce_n (flash_ce_n),
        .flash_oe_n (flash_oe_n),
        .flash_we_n (flash_we_n),
        .dbg_reading(),
        .dbg_word   ()
    );

    // =====================================================================
    // Flash model — 4 chips, all share address, data is 64-bit
    // =====================================================================

    reg [FLASH_DW-1:0] flash_mem [0:N_CHIPS-1][0:(1<<FLASH_AW)-1];
    reg [BUS_W-1:0] flash_out;

    integer fi, fj;
    initial begin
        for (fi = 0; fi < N_CHIPS; fi = fi + 1)
            for (fj = 0; fj < (1<<FLASH_AW); fj = fj + 1)
                flash_mem[fi][fj] = (fi[3:0] << 12) | (fj[11:0]);
    end

    // Read: all 4 chips output simultaneously
    integer ri;
    always @(*) begin
        flash_out = {BUS_W{1'bz}};
        if (!(&flash_ce_n) && !flash_oe_n && flash_we_n) begin
            for (ri = 0; ri < N_CHIPS; ri = ri + 1)
                flash_out[ri*FLASH_DW +: FLASH_DW] = flash_mem[ri][flash_a];
        end
    end

    assign flash_dq = (!(&flash_ce_n) && !flash_oe_n && flash_we_n) ?
                       flash_out : {BUS_W{1'bz}};

    // Write: capture on WE# rising edge
    always @(posedge flash_we_n) begin
        if (!(&flash_ce_n)) begin : wr_cap
            integer wi;
            for (wi = 0; wi < N_CHIPS; wi = wi + 1)
                flash_mem[wi][flash_a] <= flash_dq[wi*FLASH_DW +: FLASH_DW];
        end
    end

    // =====================================================================
    // Clock: 30MHz
    // =====================================================================

    initial clk = 0;
    always #16.667 clk = ~clk;

    // =====================================================================
    // Test
    // =====================================================================

    integer pass_cnt, fail_cnt;
    integer recv_cnt;
    reg [BUS_W-1:0] recv_data [0:255];

    task read_page;
        input [PAGE_ADDR_W-1:0] pa;
        begin
            recv_cnt = 0;
            @(posedge clk);
            page_addr <= pa;
            rw_n      <= 1'b1;
            start     <= 1'b1;
            @(posedge clk);
            start     <= 1'b0;

            while (!page_done) begin
                @(posedge clk);
                if (dout_valid) begin
                    recv_data[recv_cnt] = dout;
                    recv_cnt = recv_cnt + 1;
                end
            end
            @(posedge clk);
            if (dout_valid) begin
                recv_data[recv_cnt] = dout;
                recv_cnt = recv_cnt + 1;
            end
            wait(ready);
            @(posedge clk);
        end
    endtask

    function [BUS_W-1:0] expected_word;
        input [PAGE_ADDR_W-1:0] pa;
        input integer word_off;
        reg [FLASH_AW-1:0] fa;
        integer c;
        begin
            fa = {pa, word_off[WORD_OFFSET_W-1:0]};
            expected_word = {BUS_W{1'b0}};
            for (c = 0; c < N_CHIPS; c = c + 1)
                expected_word[c*FLASH_DW +: FLASH_DW] =
                    (c[3:0] << 12) | (fa & 12'hFFF);
        end
    endfunction

    initial begin
        $dumpfile("tb_flash_page_dev.vcd");
        $dumpvars(0, tb_flash_page_dev);

        pass_cnt  = 0;
        fail_cnt  = 0;
        rst_n     = 0;
        page_addr = 0;
        rw_n      = 1;
        start     = 0;
        din       = 0;
        din_valid = 0;

        repeat(5) @(posedge clk);
        rst_n = 1;
        repeat(3) @(posedge clk);

        $display("=== Flash Page Device Testbench ===");
        $display("Chips: %0d, Page size: %0d bytes, Words/page: %0d",
                 N_CHIPS, PAGE_BYTES, WORDS_PER_PAGE);
        $display("");

        // --- Test 1: Read page 0 ---
        $display("--- Read page 0 ---");
        read_page(4'd0);
        $display("Received %0d words (%0d bytes)", recv_cnt, recv_cnt * 8);

        if (recv_cnt == WORDS_PER_PAGE) begin
            $display("PASS: correct word count");
            pass_cnt = pass_cnt + 1;
        end else begin
            $display("FAIL: expected %0d words, got %0d", WORDS_PER_PAGE, recv_cnt);
            fail_cnt = fail_cnt + 1;
        end

        begin : verify_page0
            integer w;
            reg [BUS_W-1:0] exp;
            for (w = 0; w < WORDS_PER_PAGE; w = w + 1) begin
                exp = expected_word(4'd0, w);
                if (w < recv_cnt) begin
                    if (recv_data[w] === exp) begin
                        pass_cnt = pass_cnt + 1;
                        if (w < 4)
                            $display("PASS: word[%0d] = %016h", w, recv_data[w]);
                    end else begin
                        fail_cnt = fail_cnt + 1;
                        $display("FAIL: word[%0d] exp=%016h got=%016h", w, exp, recv_data[w]);
                    end
                end
            end
        end

        // --- Test 2: Read page 7 ---
        $display("");
        $display("--- Read page 7 ---");
        read_page(4'd7);
        $display("Received %0d words", recv_cnt);

        begin : verify_page7
            integer w;
            reg [BUS_W-1:0] exp;
            for (w = 0; w < WORDS_PER_PAGE; w = w + 1) begin
                exp = expected_word(4'd7, w);
                if (w < recv_cnt) begin
                    if (recv_data[w] === exp)
                        pass_cnt = pass_cnt + 1;
                    else begin
                        fail_cnt = fail_cnt + 1;
                        $display("FAIL: page7 word[%0d] exp=%016h got=%016h",
                                 w, exp, recv_data[w]);
                    end
                end
            end
        end

        // --- Test 3: Read page 15 (last) ---
        $display("");
        $display("--- Read page 15 ---");
        read_page(4'd15);
        $display("Received %0d words", recv_cnt);

        begin : verify_page15
            integer w;
            reg [BUS_W-1:0] exp;
            for (w = 0; w < WORDS_PER_PAGE; w = w + 1) begin
                exp = expected_word(4'd15, w);
                if (w < recv_cnt) begin
                    if (recv_data[w] === exp)
                        pass_cnt = pass_cnt + 1;
                    else begin
                        fail_cnt = fail_cnt + 1;
                        $display("FAIL: page15 word[%0d] exp=%016h got=%016h",
                                 w, exp, recv_data[w]);
                    end
                end
            end
        end

        // --- Test 4: Byte count ---
        $display("");
        if (recv_cnt * (BUS_W/8) == PAGE_BYTES) begin
            $display("PASS: page = %0d bytes", recv_cnt * (BUS_W/8));
            pass_cnt = pass_cnt + 1;
        end else begin
            $display("FAIL: page should be %0d bytes, got %0d",
                     PAGE_BYTES, recv_cnt * (BUS_W/8));
            fail_cnt = fail_cnt + 1;
        end

        // --- Summary ---
        $display("");
        $display("=== Results: %0d PASS, %0d FAIL ===", pass_cnt, fail_cnt);
        if (fail_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        #200;
        $finish;
    end

    initial begin
        #5000000;
        $display("TIMEOUT");
        $finish;
    end

endmodule
