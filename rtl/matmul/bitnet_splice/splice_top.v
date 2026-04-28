// Auto-generated: 4-chip BitNet splice
// Total tiles: 24
// Total layers: 24
// Pipeline latency: 28 clocks (tiles + chip boundaries)
// Throughput: 133M inf/sec (pipelined)
// Cost: 4 × HX8K @ £4 = £16

module splice_top (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        valid_in,
    input  wire [7:0]  x_in [0:7],
    output wire        valid_out,
    output wire [7:0]  y_out [0:7]
);

    wire [7:0] bus_1 [0:7];
    wire bus_1_valid;
    wire [7:0] bus_2 [0:7];
    wire bus_2_valid;
    wire [7:0] bus_3 [0:7];
    wire bus_3_valid;

    chip_0 chip_0_inst (
        .clk(clk), .rst_n(rst_n),
        .chip_valid_in(valid_in), .chip_x_in(x_in),
        .chip_valid_out(bus_1_valid), .chip_y_out(bus_1)
    );
    chip_1 chip_1_inst (
        .clk(clk), .rst_n(rst_n),
        .chip_valid_in(bus_1_valid), .chip_x_in(bus_1),
        .chip_valid_out(bus_2_valid), .chip_y_out(bus_2)
    );
    chip_2 chip_2_inst (
        .clk(clk), .rst_n(rst_n),
        .chip_valid_in(bus_2_valid), .chip_x_in(bus_2),
        .chip_valid_out(bus_3_valid), .chip_y_out(bus_3)
    );
    chip_3 chip_3_inst (
        .clk(clk), .rst_n(rst_n),
        .chip_valid_in(bus_3_valid), .chip_x_in(bus_3),
        .chip_valid_out(valid_out), .chip_y_out(y_out)
    );

endmodule
