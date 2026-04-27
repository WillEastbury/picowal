// tb_switched_matmul.v — Testbench for switched matmul engine
// Verifies correctness of BRAM-lookup matmul against arithmetic reference

`timescale 1ns / 1ps

module tb_switched_matmul;

    parameter N_ROWS  = 4;
    parameter DOT_LEN = 8;

    reg        clk, rst_n;
    reg        compute;
    reg  [7:0] x [0:DOT_LEN-1];

    reg  [$clog2(N_ROWS)-1:0] wl_row;
    reg  [2:0]                 wl_pos;
    reg  [7:0]                 wl_addr;
    reg  [15:0]                wl_data;
    reg                        wl_we;

    wire [18:0] result [0:N_ROWS-1];
    wire [N_ROWS-1:0] result_valid;

    switched_matmul #(.N_ROWS(N_ROWS), .DOT_LEN(DOT_LEN)) dut (
        .clk          (clk),
        .rst_n        (rst_n),
        .compute      (compute),
        .x            (x),
        .wl_row       (wl_row),
        .wl_pos       (wl_pos),
        .wl_addr      (wl_addr),
        .wl_data      (wl_data),
        .wl_we        (wl_we),
        .result       (result),
        .result_valid (result_valid),
        .row_group    (0)
    );

    // Clock: 133MHz
    initial clk = 0;
    always #3.76 clk = ~clk;

    // Reference weight matrix
    reg signed [7:0] W [0:N_ROWS-1][0:DOT_LEN-1];

    integer r, k, v;
    reg signed [7:0] sv;
    reg signed [15:0] prod;

    // Expected results
    reg signed [18:0] expected [0:N_ROWS-1];

    initial begin
        $dumpfile("tb_switched_matmul.vcd");
        $dumpvars(0, tb_switched_matmul);

        rst_n   = 0;
        compute = 0;
        wl_we   = 0;

        // Initialise inputs
        for (k = 0; k < DOT_LEN; k = k + 1)
            x[k] = 0;

        #20 rst_n = 1;
        #10;

        // ---- Test 1: Identity-like weights ----
        // W = [[1,0,0,0,0,0,0,0],
        //      [0,2,0,0,0,0,0,0],
        //      [0,0,-1,0,0,0,0,0],
        //      [0,0,0,0,0,0,0,127]]

        for (r = 0; r < N_ROWS; r = r + 1)
            for (k = 0; k < DOT_LEN; k = k + 1)
                W[r][k] = 0;

        W[0][0] =  1;
        W[1][1] =  2;
        W[2][2] = -1;
        W[3][7] =  127;

        // Precompute and load weight tables
        for (r = 0; r < N_ROWS; r = r + 1) begin
            for (k = 0; k < DOT_LEN; k = k + 1) begin
                for (v = 0; v < 256; v = v + 1) begin
                    sv   = v[7:0];
                    prod = W[r][k] * sv;
                    @(posedge clk);
                    wl_row  <= r;
                    wl_pos  <= k;
                    wl_addr <= v[7:0];
                    wl_data <= prod;
                    wl_we   <= 1;
                end
            end
        end
        @(posedge clk);
        wl_we <= 0;

        #20;

        // Input vector: [10, 20, 30, 40, 50, 60, 70, 80]
        x[0] = 8'd10;  x[1] = 8'd20;  x[2] = 8'd30;  x[3] = 8'd40;
        x[4] = 8'd50;  x[5] = 8'd60;  x[6] = 8'd70;  x[7] = 8'd80;

        // Compute reference
        // Row 0: 1*10 = 10
        // Row 1: 2*20 = 40
        // Row 2: -1*30 = -30
        // Row 3: 127*80 = 10160
        expected[0] = 10;
        expected[1] = 40;
        expected[2] = -30;
        expected[3] = 10160;

        @(posedge clk);
        compute <= 1;
        @(posedge clk);
        compute <= 0;

        // Wait for result (4 cycle pipeline + margin)
        repeat (10) @(posedge clk);

        // Check
        for (r = 0; r < N_ROWS; r = r + 1) begin
            if ($signed(result[r]) !== expected[r])
                $display("FAIL row %0d: got %0d, expected %0d", r, $signed(result[r]), expected[r]);
            else
                $display("PASS row %0d: %0d", r, $signed(result[r]));
        end

        // ---- Test 2: Saturated weights ----
        // W = [[127, 127, 127, 127, 127, 127, 127, 127], ...×4]
        // X = [127, 127, 127, 127, 127, 127, 127, 127]
        // Expected per row: 8 × 127 × 127 = 129,032

        for (r = 0; r < N_ROWS; r = r + 1)
            for (k = 0; k < DOT_LEN; k = k + 1)
                W[r][k] = 127;

        for (r = 0; r < N_ROWS; r = r + 1) begin
            for (k = 0; k < DOT_LEN; k = k + 1) begin
                for (v = 0; v < 256; v = v + 1) begin
                    sv   = v[7:0];
                    prod = W[r][k] * sv;
                    @(posedge clk);
                    wl_row  <= r;
                    wl_pos  <= k;
                    wl_addr <= v[7:0];
                    wl_data <= prod;
                    wl_we   <= 1;
                end
            end
        end
        @(posedge clk);
        wl_we <= 0;
        #20;

        for (k = 0; k < DOT_LEN; k = k + 1)
            x[k] = 8'd127;

        expected[0] = 129032;
        expected[1] = 129032;
        expected[2] = 129032;
        expected[3] = 129032;

        @(posedge clk);
        compute <= 1;
        @(posedge clk);
        compute <= 0;
        repeat (10) @(posedge clk);

        for (r = 0; r < N_ROWS; r = r + 1) begin
            if ($signed(result[r]) !== expected[r])
                $display("FAIL row %0d: got %0d, expected %0d", r, $signed(result[r]), expected[r]);
            else
                $display("PASS row %0d: %0d", r, $signed(result[r]));
        end

        $display("--- Tests complete ---");
        $finish;
    end

endmodule
