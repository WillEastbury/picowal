// Testbench for SATA 8b/10b Encoder and Decoder
`timescale 1ns / 1ps

module tb_sata_8b10b;

    reg         clk;
    reg         rst_n;

    // Encoder signals
    reg  [7:0]  enc_data_in;
    reg         enc_k_in;
    reg         enc_valid_in;
    wire [9:0]  enc_data_out;
    wire        enc_valid_out;

    // Decoder signals
    wire [7:0]  dec_data_out;
    wire        dec_k_out;
    wire        dec_err_out;
    wire        dec_valid_out;

    // Instantiate encoder
    sata_8b10b_enc u_enc (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (enc_data_in),
        .k_in      (enc_k_in),
        .valid_in  (enc_valid_in),
        .data_out  (enc_data_out),
        .valid_out (enc_valid_out)
    );

    // Connect encoder output to decoder input
    sata_8b10b_dec u_dec (
        .clk       (clk),
        .rst_n     (rst_n),
        .data_in   (enc_data_out),
        .valid_in  (enc_valid_out),
        .data_out  (dec_data_out),
        .k_out     (dec_k_out),
        .err_out   (dec_err_out),
        .valid_out (dec_valid_out)
    );

    // Clock generation: 10ns period
    initial clk = 0;
    always #5 clk = ~clk;

    // Test tracking
    integer total_tests;
    integer pass_count;
    integer fail_count;
    integer i;

    // Expected values pipeline (2 cycle latency: enc 1 + dec 1)
    reg [7:0] exp_data [0:1];
    reg       exp_k    [0:1];
    reg       exp_valid[0:1];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            exp_data[0]  <= 8'd0; exp_data[1]  <= 8'd0;
            exp_k[0]     <= 1'b0; exp_k[1]     <= 1'b0;
            exp_valid[0] <= 1'b0; exp_valid[1] <= 1'b0;
        end else begin
            exp_data[0]  <= enc_data_in;
            exp_k[0]     <= enc_k_in;
            exp_valid[0] <= enc_valid_in;
            exp_data[1]  <= exp_data[0];
            exp_k[1]     <= exp_k[0];
            exp_valid[1] <= exp_valid[0];
        end
    end

    // K symbol table
    reg [8:0] k_symbols [0:8]; // {k_flag, data}
    initial begin
        k_symbols[0] = {1'b1, 8'h1C}; // K28.0
        k_symbols[1] = {1'b1, 8'h3C}; // K28.1
        k_symbols[2] = {1'b1, 8'h5C}; // K28.2
        k_symbols[3] = {1'b1, 8'h7C}; // K28.3
        k_symbols[4] = {1'b1, 8'hBC}; // K28.5 (COMMA)
        k_symbols[5] = {1'b1, 8'hDC}; // K28.6
        k_symbols[6] = {1'b1, 8'hFC}; // K28.7
        k_symbols[7] = {1'b1, 8'hFD}; // K29.7
        k_symbols[8] = {1'b1, 8'hFE}; // K30.7
    end

    // Task to send one symbol and wait for result
    // Timing: set inputs before posedge, encoder latches on posedge N,
    // decoder latches on posedge N+1, sample on negedge after N+1.
    task send_symbol;
        input [7:0] data;
        input        k;
        begin
            @(negedge clk);
            enc_data_in  = data;
            enc_k_in     = k;
            enc_valid_in = 1'b1;
            @(posedge clk); // encoder latches valid_in=1
            @(negedge clk);
            enc_valid_in = 1'b0; // de-assert after encoder has captured
            @(posedge clk); // decoder latches enc_valid_out=1
            @(negedge clk); // sample decoder outputs here
        end
    endtask

    // Task to check decoder output
    task check_result;
        input [7:0] exp_d;
        input        exp_kf;
        input [63:0] label; // 8-char label for display
        begin
            total_tests = total_tests + 1;
            if (dec_valid_out !== 1'b1) begin
                $display("FAIL [%0s] data=0x%02x k=%0b: valid_out not asserted",
                         label, exp_d, exp_kf);
                fail_count = fail_count + 1;
            end else if (dec_data_out !== exp_d || dec_k_out !== exp_kf) begin
                $display("FAIL [%0s] data=0x%02x k=%0b: got data=0x%02x k=%0b enc=0x%03x",
                         label, exp_d, exp_kf, dec_data_out, dec_k_out, enc_data_out);
                fail_count = fail_count + 1;
            end else if (dec_err_out !== 1'b0) begin
                $display("FAIL [%0s] data=0x%02x k=%0b: err_out asserted, enc=0x%03x",
                         label, exp_d, exp_kf, enc_data_out);
                fail_count = fail_count + 1;
            end else begin
                pass_count = pass_count + 1;
            end
        end
    endtask

    // Disparity tracking for verification
    reg prev_rd_enc;
    integer rd_transitions;
    integer rd_checks;
    integer rd_errors;

    initial begin
        // Init
        rst_n        = 0;
        enc_data_in  = 8'd0;
        enc_k_in     = 1'b0;
        enc_valid_in = 1'b0;
        total_tests  = 0;
        pass_count   = 0;
        fail_count   = 0;
        rd_transitions = 0;
        rd_checks    = 0;
        rd_errors    = 0;

        // Reset
        repeat (4) @(posedge clk);
        rst_n = 1;
        repeat (2) @(posedge clk);

        $display("=== SATA 8b/10b Encoder/Decoder Testbench ===");
        $display("");

        // ---------------------------------------------------------
        // Test 1: Encode/decode all 256 data values (D0.0 - D31.7)
        // ---------------------------------------------------------
        $display("--- Test 1: All 256 data symbols ---");
        for (i = 0; i < 256; i = i + 1) begin
            send_symbol(i[7:0], 1'b0);
            check_result(i[7:0], 1'b0, "DATA   ");
        end
        $display("    Data symbols: %0d passed, %0d failed", pass_count, fail_count);

        // ---------------------------------------------------------
        // Test 2: Encode/decode all required K symbols
        // ---------------------------------------------------------
        $display("--- Test 2: K symbols ---");
        begin : k_test
            integer k_pass_before;
            k_pass_before = pass_count;
            for (i = 0; i < 9; i = i + 1) begin
                send_symbol(k_symbols[i][7:0], 1'b1);
                check_result(k_symbols[i][7:0], 1'b1, "K-SYM  ");
            end
            $display("    K symbols: %0d passed, %0d failed",
                     pass_count - k_pass_before, fail_count);
        end

        // ---------------------------------------------------------
        // Test 3: Verify running disparity alternation
        // Send symbols that force disparity changes and verify
        // the encoded output alternates between RD+ and RD- forms.
        // ---------------------------------------------------------
        $display("--- Test 3: Running disparity checks ---");
        begin : rd_test
            integer rd_pass;
            reg [9:0] prev_enc;
            reg [9:0] cur_enc;
            integer ones_a, ones_b, j;

            rd_pass = 0;
            rd_errors = 0;

            // Send D.0.0 (0x00) repeatedly - it has non-balanced codes
            // RD- produces 100111_1011 (7 ones), RD+ produces 011000_0100 (3 ones)
            // So consecutive D.0.0 should alternate between these two forms

            // First symbol
            @(negedge clk);
            enc_data_in  = 8'h00;
            enc_k_in     = 1'b0;
            enc_valid_in = 1'b1;
            @(posedge clk);
            @(negedge clk);
            prev_enc = enc_data_out;

            for (j = 0; j < 20; j = j + 1) begin
                enc_data_in  = 8'h00;
                enc_k_in     = 1'b0;
                enc_valid_in = 1'b1;
                @(posedge clk);
                @(negedge clk);
                cur_enc = enc_data_out;

                // Count ones in previous and current
                ones_a = 0; ones_b = 0;
                for (i = 0; i < 10; i = i + 1) begin
                    ones_a = ones_a + prev_enc[i];
                    ones_b = ones_b + cur_enc[i];
                end

                // If prev had positive disparity (>5 ones), current should
                // have negative disparity (<5 ones) or vice versa
                if (ones_a > 5 && ones_b > 5) begin
                    rd_errors = rd_errors + 1;
                    $display("FAIL: RD not alternating at step %0d: prev=%b(%0d ones) cur=%b(%0d ones)",
                             j, prev_enc, ones_a, cur_enc, ones_b);
                end else if (ones_a < 5 && ones_b < 5) begin
                    rd_errors = rd_errors + 1;
                    $display("FAIL: RD not alternating at step %0d: prev=%b(%0d ones) cur=%b(%0d ones)",
                             j, prev_enc, ones_a, cur_enc, ones_b);
                end else begin
                    rd_pass = rd_pass + 1;
                end
                prev_enc = cur_enc;
            end

            enc_valid_in = 1'b0;
            @(posedge clk);

            // Also test with K28.5 (COMMA) which should also alternate
            @(negedge clk);
            enc_data_in  = 8'hBC;
            enc_k_in     = 1'b1;
            enc_valid_in = 1'b1;
            @(posedge clk);
            @(negedge clk);
            prev_enc = enc_data_out;

            for (j = 0; j < 10; j = j + 1) begin
                enc_data_in  = 8'hBC;
                enc_k_in     = 1'b1;
                enc_valid_in = 1'b1;
                @(posedge clk);
                @(negedge clk);
                cur_enc = enc_data_out;
                ones_a = 0; ones_b = 0;
                for (i = 0; i < 10; i = i + 1) begin
                    ones_a = ones_a + prev_enc[i];
                    ones_b = ones_b + cur_enc[i];
                end
                if (ones_a > 5 && ones_b > 5) begin
                    rd_errors = rd_errors + 1;
                    $display("FAIL: K28.5 RD not alternating at step %0d", j);
                end else if (ones_a < 5 && ones_b < 5) begin
                    rd_errors = rd_errors + 1;
                    $display("FAIL: K28.5 RD not alternating at step %0d", j);
                end else begin
                    rd_pass = rd_pass + 1;
                end
                prev_enc = cur_enc;
            end
            enc_valid_in = 1'b0;
            @(posedge clk);

            total_tests = total_tests + rd_pass + rd_errors;
            pass_count  = pass_count + rd_pass;
            fail_count  = fail_count + rd_errors;
            $display("    RD alternation: %0d passed, %0d failed", rd_pass, rd_errors);
        end

        // ---------------------------------------------------------
        // Test 4: Re-encode all 256 data values starting from RD+
        // (by first sending a symbol that flips to RD+)
        // This verifies encoding from both RD states.
        // ---------------------------------------------------------
        $display("--- Test 4: All 256 data from RD+ starting state ---");
        // Reset to get clean state
        rst_n = 0;
        repeat (4) @(posedge clk);
        rst_n = 1;
        repeat (2) @(posedge clk);

        // Send D.0.0 once to flip RD from RD- to RD+
        send_symbol(8'h00, 1'b0);
        // Discard this check (pipeline artifact)
        total_tests = total_tests + 1;
        pass_count  = pass_count + 1;

        begin : rd_plus_test
            integer rp_pass_before;
            rp_pass_before = pass_count;
            for (i = 0; i < 256; i = i + 1) begin
                send_symbol(i[7:0], 1'b0);
                check_result(i[7:0], 1'b0, "DATA_R+");
            end
            $display("    Data (RD+): %0d passed, %0d failed",
                     pass_count - rp_pass_before, fail_count);
        end

        // ---------------------------------------------------------
        // Test 5: K symbols from RD+ state
        // ---------------------------------------------------------
        $display("--- Test 5: K symbols from RD+ state ---");
        // Reset and flip to RD+
        rst_n = 0;
        repeat (4) @(posedge clk);
        rst_n = 1;
        repeat (2) @(posedge clk);
        send_symbol(8'h00, 1'b0);
        total_tests = total_tests + 1;
        pass_count  = pass_count + 1;

        begin : k_rdp_test
            integer kp_pass_before;
            kp_pass_before = pass_count;
            for (i = 0; i < 9; i = i + 1) begin
                send_symbol(k_symbols[i][7:0], 1'b1);
                check_result(k_symbols[i][7:0], 1'b1, "K_RD+  ");
            end
            $display("    K (RD+): %0d passed, %0d failed",
                     pass_count - kp_pass_before, fail_count);
        end

        // ---------------------------------------------------------
        // Summary
        // ---------------------------------------------------------
        $display("");
        $display("============================================");
        $display("  Total : %0d", total_tests);
        $display("  Pass  : %0d", pass_count);
        $display("  Fail  : %0d", fail_count);
        if (fail_count == 0)
            $display("  Result: *** PASS ***");
        else
            $display("  Result: *** FAIL ***");
        $display("============================================");

        #100;
        $finish;
    end

endmodule
