// Testbench for sata_crc32 and sata_scrambler
`timescale 1ns / 1ps

module tb_sata_crc_scrambler;

    reg         clk;
    reg         rst_n;

    // CRC signals
    reg         crc_init;
    reg  [31:0] crc_data;
    reg         crc_valid;
    wire [31:0] crc_out;

    // Scrambler signals (instance A: scramble, instance B: descramble)
    reg         scr_init;
    reg  [31:0] scr_data_in;
    reg         scr_valid;
    wire [31:0] scr_data_out;

    reg         dscr_init;
    reg  [31:0] dscr_data_in;
    reg         dscr_valid;
    wire [31:0] dscr_data_out;

    integer pass_count;
    integer fail_count;
    integer test_num;

    sata_crc32 u_crc (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (crc_init),
        .data_in  (crc_data),
        .valid    (crc_valid),
        .crc_out  (crc_out)
    );

    sata_scrambler u_scram (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (scr_init),
        .data_in  (scr_data_in),
        .valid    (scr_valid),
        .data_out (scr_data_out)
    );

    sata_scrambler u_descram (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (dscr_init),
        .data_in  (dscr_data_in),
        .valid    (dscr_valid),
        .data_out (dscr_data_out)
    );

    // Clock generation
    initial clk = 0;
    always #5 clk = ~clk;

    task check;
        input [255:0] name;
        input [31:0]  actual;
        input [31:0]  expected;
        begin
            if (actual === expected) begin
                $display("  PASS: %0s = 0x%08X", name, actual);
                pass_count = pass_count + 1;
            end else begin
                $display("  FAIL: %0s = 0x%08X (expected 0x%08X)", name, actual, expected);
                fail_count = fail_count + 1;
            end
        end
    endtask

    task crc_feed;
        input [31:0] data;
        begin
            @(posedge clk);
            crc_data  <= data;
            crc_valid <= 1;
            @(posedge clk);
            crc_valid <= 0;
        end
    endtask

    reg [31:0] scr_captured;
    reg [31:0] dscr_captured;

    // Capture scrambler outputs at posedge when valid — before LFSR NBA update
    always @(posedge clk) begin
        if (scr_valid)
            scr_captured <= scr_data_out;
        if (dscr_valid)
            dscr_captured <= dscr_data_out;
    end

    task scr_feed;
        input [31:0] data;
        begin
            @(posedge clk);
            scr_data_in <= data;
            scr_valid   <= 1;
            @(posedge clk);
            scr_valid   <= 0;
            @(posedge clk); // wait for scr_captured to settle
        end
    endtask

    initial begin
        pass_count = 0;
        fail_count = 0;
        test_num   = 0;

        rst_n      = 0;
        crc_init   = 0;
        crc_data   = 0;
        crc_valid  = 0;
        scr_init   = 0;
        scr_data_in = 0;
        scr_valid  = 0;
        dscr_init  = 0;
        dscr_data_in = 0;
        dscr_valid = 0;

        // Reset
        #20;
        rst_n = 1;
        @(posedge clk);

        // ============================================================
        // TEST 1: CRC of single DWORD 0x00000000
        // ============================================================
        test_num = 1;
        $display("\nTest %0d: CRC of single DWORD 0x00000000", test_num);
        @(posedge clk);
        crc_init <= 1;
        @(posedge clk);
        crc_init <= 0;
        @(posedge clk);

        crc_feed(32'h00000000);
        @(posedge clk);
        check("CRC(0x00000000)", crc_out, 32'h33943510);

        // ============================================================
        // TEST 2: CRC of single DWORD 0x12345678
        // ============================================================
        test_num = 2;
        $display("\nTest %0d: CRC of single DWORD 0x12345678", test_num);
        @(posedge clk);
        crc_init <= 1;
        @(posedge clk);
        crc_init <= 0;
        @(posedge clk);

        crc_feed(32'h12345678);
        @(posedge clk);
        check("CRC(0x12345678)", crc_out, 32'h408EA161);

        // ============================================================
        // TEST 3: CRC of two DWORDs — verify init resets between frames
        // ============================================================
        test_num = 3;
        $display("\nTest %0d: CRC of two DWORDs [0x12345678, 0xDEADBEEF]", test_num);
        @(posedge clk);
        crc_init <= 1;
        @(posedge clk);
        crc_init <= 0;
        @(posedge clk);

        crc_feed(32'h12345678);
        crc_feed(32'hDEADBEEF);
        @(posedge clk);
        check("CRC(0x12345678,0xDEADBEEF)", crc_out, 32'h2FFCD061);

        // ============================================================
        // TEST 4: CRC init resets properly — repeat test 1 after test 3
        // ============================================================
        test_num = 4;
        $display("\nTest %0d: CRC init reset between frames", test_num);
        @(posedge clk);
        crc_init <= 1;
        @(posedge clk);
        crc_init <= 0;
        @(posedge clk);

        check("CRC after init", crc_out, 32'h52325032);

        crc_feed(32'h00000000);
        @(posedge clk);
        check("CRC(0x00000000) again", crc_out, 32'h33943510);

        // ============================================================
        // TEST 5: CRC of 4-DWORD FIS-like payload
        // ============================================================
        test_num = 5;
        $display("\nTest %0d: CRC of 4-DWORD payload", test_num);
        @(posedge clk);
        crc_init <= 1;
        @(posedge clk);
        crc_init <= 0;
        @(posedge clk);

        crc_feed(32'h00000027);
        crc_feed(32'h00A08000);
        crc_feed(32'h00000000);
        crc_feed(32'h00000001);
        @(posedge clk);
        check("CRC(FIS payload)", crc_out, 32'hE5ED0034);

        // ============================================================
        // TEST 6: Scrambler deterministic context (zero-data output)
        // First 4 DWORDs should match SATA spec values
        // ============================================================
        test_num = 6;
        $display("\nTest %0d: Scrambler deterministic context from seed", test_num);
        @(posedge clk);
        scr_init <= 1;
        @(posedge clk);
        scr_init <= 0;
        @(posedge clk);

        // Feed zeros — output is raw LFSR context
        scr_feed(32'h00000000);
        check("Context DWORD 0", scr_captured, 32'hC2D2768D);

        scr_feed(32'h00000000);
        check("Context DWORD 1", scr_captured, 32'h1F26B368);

        scr_feed(32'h00000000);
        check("Context DWORD 2", scr_captured, 32'hA508436C);

        scr_feed(32'h00000000);
        check("Context DWORD 3", scr_captured, 32'h3452D354);

        // ============================================================
        // TEST 7: Scrambler LFSR resets on init
        // ============================================================
        test_num = 7;
        $display("\nTest %0d: Scrambler LFSR resets on init", test_num);
        @(posedge clk);
        scr_init <= 1;
        @(posedge clk);
        scr_init <= 0;
        @(posedge clk);

        // After re-init, context DWORD 0 should repeat
        scr_feed(32'h00000000);
        check("Context DWORD 0 after reinit", scr_captured, 32'hC2D2768D);

        // ============================================================
        // TEST 8: Scramble then descramble roundtrip
        // ============================================================
        test_num = 8;
        $display("\nTest %0d: Scramble/descramble roundtrip", test_num);

        // Reset both scrambler instances
        @(posedge clk);
        scr_init  <= 1;
        dscr_init <= 1;
        @(posedge clk);
        scr_init  <= 0;
        dscr_init <= 0;
        @(posedge clk);

        // Scramble 4 DWORDs
        begin : roundtrip_block
            reg [31:0] original [0:3];
            reg [31:0] scrambled [0:3];
            integer j;

            original[0] = 32'h12345678;
            original[1] = 32'hDEADBEEF;
            original[2] = 32'hCAFEBABE;
            original[3] = 32'h00000000;

            for (j = 0; j < 4; j = j + 1) begin
                @(posedge clk);
                scr_data_in <= original[j];
                scr_valid   <= 1;
                @(posedge clk);
                scr_valid   <= 0;
                @(posedge clk); // wait for scr_captured
                scrambled[j] = scr_captured;
            end

            check("Scrambled[0]", scrambled[0], 32'hD0E620F5);

            // Now descramble
            for (j = 0; j < 4; j = j + 1) begin
                @(posedge clk);
                dscr_data_in <= scrambled[j];
                dscr_valid   <= 1;
                @(posedge clk);
                dscr_valid   <= 0;
                @(posedge clk); // wait for dscr_captured
                check("Roundtrip", dscr_captured, original[j]);
            end
        end

        // ============================================================
        // TEST 9: Scrambler with non-zero data
        // ============================================================
        test_num = 9;
        $display("\nTest %0d: Scrambler with known data", test_num);
        @(posedge clk);
        scr_init <= 1;
        @(posedge clk);
        scr_init <= 0;
        @(posedge clk);

        // data XOR context: 0x12345678 XOR 0xC2D2768D = 0xD0E620F5
        scr_feed(32'h12345678);
        check("Scramble(0x12345678)", scr_captured, 32'hD0E620F5);

        // ============================================================
        // Summary
        // ============================================================
        $display("\n========================================");
        $display("  Tests passed: %0d", pass_count);
        $display("  Tests failed: %0d", fail_count);
        if (fail_count == 0)
            $display("  ALL TESTS PASSED");
        else
            $display("  SOME TESTS FAILED");
        $display("========================================\n");

        $finish;
    end

endmodule
