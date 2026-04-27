// switched_dot8.v — 8-element dot product via switched multiplies
// Computes: result = sum(weight[k] * x[k]) for k=0..7
// ALL 8 lookups happen simultaneously. Adder tree reduces.
//
// Resources: 8 BRAM blocks + ~120 LUTs (adder tree)
// Latency: 1 cycle (BRAM read) + 3 cycles (adder tree) = 4 cycles total
// Throughput: 1 dot product per cycle (fully pipelined)

module switched_dot8 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        in_valid,

    // Broadcast input vector: 8 × INT8 presented simultaneously
    input  wire [7:0]  x0, x1, x2, x3, x4, x5, x6, x7,

    // Weight loading interface (shared bus, active one-at-a-time)
    input  wire [2:0]  w_sel,      // which of the 8 weight tables to write
    input  wire [7:0]  w_addr,     // address within table (0-255)
    input  wire [15:0] w_data,     // precomputed product value
    input  wire        w_we,       // write enable

    // Result
    output wire [18:0] dot_out,    // signed 19-bit dot product
    output wire        out_valid
);

    // --- 8 switched multiplies: BRAM lookup, zero arithmetic ---

    wire [15:0] prod [0:7];
    wire [7:0]  we_decode = (w_we) ? (8'b1 << w_sel) : 8'b0;

    genvar i;
    generate
        for (i = 0; i < 8; i = i + 1) begin : mult
            wire [7:0] x_mux;
            // Select the right input for this position
            assign x_mux = (i == 0) ? x0 :
                           (i == 1) ? x1 :
                           (i == 2) ? x2 :
                           (i == 3) ? x3 :
                           (i == 4) ? x4 :
                           (i == 5) ? x5 :
                           (i == 6) ? x6 : x7;

            switched_multiply sm (
                .clk     (clk),
                .x_in    (x_mux),
                .w_addr  (w_addr),
                .w_data  (w_data),
                .w_we    (we_decode[i]),
                .product (prod[i])
            );
        end
    endgenerate

    // Pipeline register: capture BRAM output + valid
    reg [15:0] prod_r [0:7];
    reg        prod_valid;

    integer j;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            prod_valid <= 0;
        end else begin
            for (j = 0; j < 8; j = j + 1)
                prod_r[j] <= prod[j];
            prod_valid <= in_valid;
        end
    end

    // --- Adder tree: 8 products → 1 sum in 3 pipelined stages ---

    adder_tree_8 tree (
        .clk       (clk),
        .rst_n     (rst_n),
        .in_valid  (prod_valid),
        .p0 (prod_r[0]), .p1 (prod_r[1]),
        .p2 (prod_r[2]), .p3 (prod_r[3]),
        .p4 (prod_r[4]), .p5 (prod_r[5]),
        .p6 (prod_r[6]), .p7 (prod_r[7]),
        .sum_out   (dot_out),
        .out_valid (out_valid)
    );

endmodule
