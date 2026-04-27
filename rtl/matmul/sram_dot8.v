// sram_dot8.v — 8-element dot product using 8 external SRAM chips
//
// Each SRAM holds lookup tables for one column position of the weight matrix.
// All 8 SRAMs read simultaneously (async, 10ns).
// FPGA adder tree reduces the 8 products to one 19-bit dot product.
//
// Resources: 8 × IS61WV25616BLL ($16 total) + ~120 LUTs adder tree
// Throughput: 1 dot product every 2 clocks @ 133MHz = 66M dot-products/sec
// Latency: ~25ns (10ns SRAM + 15ns adder tree)
//
// The weight_sel input selects WHICH row's weights to use.
// Each SRAM stores 1024 weight tables → supports 1024 output rows
// without any reloading. Just change weight_sel.
//
// For a 1024×8 matmul: 1024 rows × 2 clocks = 2048 clocks = 15.4μs
// = 1024 × 8 × 2 = 16,384 ops / 15.4μs = 1.06 GOPS per dot unit

module sram_dot8 (
    input  wire        clk,
    input  wire        rst_n,

    // Inference
    input  wire        compute,        // pulse: start dot product
    input  wire [9:0]  weight_sel,     // row index (0-1023) — same for all 8 SRAMs
    input  wire [7:0]  x [0:7],        // input vector (8 bytes, broadcast)

    // Weight loading (directly to SRAMs)
    input  wire        mode_write,
    input  wire [2:0]  wr_sram_sel,    // which of 8 SRAMs to write
    input  wire [17:0] wr_addr,
    input  wire [15:0] wr_data,
    input  wire        wr_valid,

    // Physical SRAM interfaces (active directly to 8 chip pin groups)
    output wire [17:0] sram_addr [0:7],
    inout  wire [15:0] sram_data [0:7],
    output wire [7:0]  sram_ce_n,
    output wire [7:0]  sram_oe_n,
    output wire [7:0]  sram_we_n,

    // Result
    output wire [18:0] dot_out,
    output wire        dot_valid
);

    wire [15:0] products [0:7];
    wire [7:0]  prod_valid;

    genvar i;
    generate
        for (i = 0; i < 8; i = i + 1) begin : sram_ch

            wire wr_this = wr_valid && (wr_sram_sel == i);

            sram_switched_multiply sm (
                .clk          (clk),
                .rst_n        (rst_n),
                .mode_write   (mode_write),
                .read_valid   (compute),
                .weight_sel   (weight_sel),
                .x_in         (x[i]),
                .wr_addr      (wr_addr),
                .wr_data      (wr_data),
                .wr_valid     (wr_this),
                .sram_addr    (sram_addr[i]),
                .sram_data    (sram_data[i]),
                .sram_ce_n    (sram_ce_n[i]),
                .sram_oe_n    (sram_oe_n[i]),
                .sram_we_n    (sram_we_n[i]),
                .product      (products[i]),
                .product_valid(prod_valid[i])
            );
        end
    endgenerate

    // Adder tree: reduce 8 × 16-bit products to 19-bit sum
    adder_tree_8 tree (
        .clk       (clk),
        .rst_n     (rst_n),
        .in_valid  (prod_valid[0]),  // all channels valid simultaneously
        .p0 (products[0]), .p1 (products[1]),
        .p2 (products[2]), .p3 (products[3]),
        .p4 (products[4]), .p5 (products[5]),
        .p6 (products[6]), .p7 (products[7]),
        .sum_out   (dot_out),
        .out_valid (dot_valid)
    );

endmodule
