// tb_flash_block_dev.v — Testbench for streaming flash block device

`timescale 1ns / 1ps

module tb_flash_block_dev;

    // Use small params for sim
    parameter N_CHIPS        = 8;
    parameter CHIPS_PER_BANK = 4;
    parameter FLASH_AW       = 10;       // 1K words per chip (tiny for sim)
    parameter FLASH_DW       = 16;
    parameter BLOCK_ADDR_W   = 4;        // 16 blocks
    parameter BUS_W          = CHIPS_PER_BANK * FLASH_DW;  // 64 bits
    parameter N_BANKS        = N_CHIPS / CHIPS_PER_BANK;   // 2
    parameter WORDS_PER_BLOCK = (1 << FLASH_AW) >> BLOCK_ADDR_W;  // 64

    reg                         clk;
    reg                         rst_n;
    reg  [BLOCK_ADDR_W-1:0]    block_addr;
    reg                         rw_n;
    reg                         start;
    wire                        ready;
    wire [BUS_W-1:0]           dout;
    wire                        dout_valid;
    wire                        block_done;
    reg  [BUS_W-1:0]           din;
    reg                         din_valid;

    wire [FLASH_AW-1:0]        flash_a;
    wire [BUS_W-1:0]           flash_dq;
    wire [N_CHIPS-1:0]         flash_ce_n;
    wire                        flash_oe_n;
    wire                        flash_we_n;
    wire [2:0]                 dbg_bank;
    wire [6:0]                 dbg_word_offset;

    // =====================================================================
    // DUT
    // =====================================================================

    flash_block_dev #(
        .N_CHIPS(N_CHIPS),
        .CHIPS_PER_BANK(CHIPS_PER_BANK),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW),
        .BLOCK_ADDR_W(BLOCK_ADDR_W)
    ) dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .block_addr     (block_addr),
        .rw_n           (rw_n),
        .start          (start),
        .ready          (ready),
        .dout           (dout),
        .dout_valid     (dout_valid),
        .block_done     (block_done),
        .din            (din),
        .din_valid      (din_valid),
        .flash_a        (flash_a),
        .flash_dq       (flash_dq),
        .flash_ce_n     (flash_ce_n),
        .flash_oe_n     (flash_oe_n),
        .flash_we_n     (flash_we_n),
        .dbg_bank       (dbg_bank),
        .dbg_word_offset(dbg_word_offset)
    );

    // =====================================================================
    // Flash memory model — 8 chips, but only 4 active at a time (bank mux)
    // Each chip: independent memory, shared address, banked CE#
    // The physical data bus is 64 bits (4 chips × 16 bits)
    // When bank 0 active: chips 0-3 drive the bus
    // When bank 1 active: chips 4-7 drive the bus
    // =====================================================================

    reg [FLASH_DW-1:0] flash_mem [0:N_CHIPS-1][0:(1<<FLASH_AW)-1];

    // Determine which bank is driving
    wire [N_BANKS-1:0] bank_active;
    genvar b;
    generate
        for (b = 0; b < N_BANKS; b = b + 1) begin : bank_detect
            // A bank is active when ALL its chips have CE# low
            assign bank_active[b] = ~|flash_ce_n[b*CHIPS_PER_BANK +: CHIPS_PER_BANK];
        end
    endgenerate

    // Build flash data output: whichever bank is enabled drives the bus
    reg [BUS_W-1:0] flash_out;
    integer bi, ci2;
    always @(*) begin
        flash_out = {BUS_W{1'bz}};
        for (bi = 0; bi < N_BANKS; bi = bi + 1) begin
            if (bank_active[bi] && !flash_oe_n && flash_we_n) begin
                for (ci2 = 0; ci2 < CHIPS_PER_BANK; ci2 = ci2 + 1) begin
                    flash_out[ci2*FLASH_DW +: FLASH_DW] =
                        flash_mem[bi*CHIPS_PER_BANK + ci2][flash_a];
                end
            end
        end
    end

    assign flash_dq = (!flash_oe_n && flash_we_n) ? flash_out : {BUS_W{1'bz}};

    // Init flash with known pattern
    integer fi, fj;
    initial begin
        for (fi = 0; fi < N_CHIPS; fi = fi + 1)
            for (fj = 0; fj < (1<<FLASH_AW); fj = fj + 1)
                flash_mem[fi][fj] = (fi[3:0] << 12) | (fj[11:0]);
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
    reg [BUS_W-1:0] recv_data [0:1023];  // capture streamed output

    task read_block;
        input [BLOCK_ADDR_W-1:0] ba;
        begin
            recv_cnt = 0;

            @(posedge clk);
            block_addr <= ba;
            rw_n       <= 1'b1;
            start      <= 1'b1;
            @(posedge clk);
            start      <= 1'b0;

            // Collect all dout_valid pulses
            while (!block_done) begin
                @(posedge clk);
                if (dout_valid) begin
                    recv_data[recv_cnt] = dout;
                    recv_cnt = recv_cnt + 1;
                end
            end

            // Might get one more on the done cycle
            @(posedge clk);
            if (dout_valid) begin
                recv_data[recv_cnt] = dout;
                recv_cnt = recv_cnt + 1;
            end

            wait(ready);
            @(posedge clk);
        end
    endtask

    // Expected: for block BA, word offset W, bank B, chip C within bank:
    //   flash_mem[B*4+C][{BA, W}] = ((B*4+C) << 12) | ({BA, W} & 0xFFF)
    // Output order: for each word_offset, cycle through banks
    // So output word index = word_offset * N_BANKS + bank_idx
    // Each output word: {chip3_data, chip2_data, chip1_data, chip0_data}

    function [BUS_W-1:0] expected_word;
        input [BLOCK_ADDR_W-1:0] ba;
        input integer word_off;
        input integer bank;
        integer c;
        reg [FLASH_AW-1:0] fa;
        begin
            fa = {ba, word_off[FLASH_AW-BLOCK_ADDR_W-1:0]};
            expected_word = {BUS_W{1'b0}};
            for (c = 0; c < CHIPS_PER_BANK; c = c + 1)
                expected_word[c*FLASH_DW +: FLASH_DW] =
                    ((bank * CHIPS_PER_BANK + c) << 12) | (fa & 12'hFFF);
        end
    endfunction

    initial begin
        $dumpfile("tb_flash_block_dev.vcd");
        $dumpvars(0, tb_flash_block_dev);

        pass_cnt = 0;
        fail_cnt = 0;
        rst_n    = 0;
        block_addr = 0;
        rw_n     = 1;
        start    = 0;
        din      = 0;
        din_valid = 0;

        repeat(5) @(posedge clk);
        rst_n = 1;
        repeat(3) @(posedge clk);

        $display("=== Flash Block Device Testbench ===");
        $display("Chips: %0d, Banks: %0d, Words/block: %0d",
                 N_CHIPS, N_BANKS, WORDS_PER_BLOCK);
        $display("Block size: %0d bytes",
                 WORDS_PER_BLOCK * (FLASH_DW/8) * N_CHIPS);
        $display("Expected output words per block: %0d",
                 WORDS_PER_BLOCK * N_BANKS);
        $display("");

        // --- Read block 0 ---
        $display("--- Reading block 0 ---");
        read_block(4'd0);
        $display("Received %0d words", recv_cnt);

        // Check first few words
        begin : check_block0
            integer w, bk, idx;
            reg [BUS_W-1:0] exp;

            idx = 0;
            for (w = 0; w < WORDS_PER_BLOCK && w < 4; w = w + 1) begin
                for (bk = 0; bk < N_BANKS; bk = bk + 1) begin
                    exp = expected_word(4'd0, w, bk);
                    if (idx < recv_cnt) begin
                        if (recv_data[idx] === exp) begin
                            pass_cnt = pass_cnt + 1;
                            if (idx < 8)
                                $display("PASS: word[%0d] w=%0d bank=%0d = %016h",
                                         idx, w, bk, recv_data[idx]);
                        end else begin
                            fail_cnt = fail_cnt + 1;
                            $display("FAIL: word[%0d] w=%0d bank=%0d exp=%016h got=%016h",
                                     idx, w, bk, exp, recv_data[idx]);
                        end
                    end
                    idx = idx + 1;
                end
            end
        end

        // --- Read block 5 ---
        $display("");
        $display("--- Reading block 5 ---");
        read_block(4'd5);
        $display("Received %0d words", recv_cnt);

        begin : check_block5
            integer w, bk, idx;
            reg [BUS_W-1:0] exp;
            idx = 0;
            for (w = 0; w < 2; w = w + 1) begin
                for (bk = 0; bk < N_BANKS; bk = bk + 1) begin
                    exp = expected_word(4'd5, w, bk);
                    if (idx < recv_cnt) begin
                        if (recv_data[idx] === exp) begin
                            pass_cnt = pass_cnt + 1;
                        end else begin
                            fail_cnt = fail_cnt + 1;
                            $display("FAIL: word[%0d] exp=%016h got=%016h",
                                     idx, exp, recv_data[idx]);
                        end
                    end
                    idx = idx + 1;
                end
            end
        end

        // --- Check total words received ---
        $display("");
        begin : check_count
            integer exp_words;
            exp_words = WORDS_PER_BLOCK * N_BANKS;  // 64 × 2 = 128
            if (recv_cnt == exp_words) begin
                $display("PASS: word count = %0d (expected %0d)", recv_cnt, exp_words);
                pass_cnt = pass_cnt + 1;
            end else begin
                $display("FAIL: word count = %0d (expected %0d)", recv_cnt, exp_words);
                fail_cnt = fail_cnt + 1;
            end
        end

        $display("");
        $display("=== Results: %0d PASS, %0d FAIL ===", pass_cnt, fail_cnt);
        if (fail_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("SOME TESTS FAILED");

        #200;
        $finish;
    end

    // Timeout
    initial begin
        #5000000;
        $display("TIMEOUT");
        $finish;
    end

endmodule
