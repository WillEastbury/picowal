// Auto-generated BitNet chain: 10 layers
// Total tiles: 10
// On HX1K: 10 × £1.80 = £18.00
// On ASIC: ~12160 LUTs = ~145K gates
// Pipeline: 10 clocks latency, 1 inference/clock

module bitnet_chain_top (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        valid_in,
    input  wire [7:0]  x_in [0:7],
    output wire        valid_out,
    output wire [15:0] y_out [0:7]
);

    wire [15:0] inter_1 [0:7];
    wire v_1;
    wire [15:0] inter_2 [0:7];
    wire v_2;
    wire [7:0] rq_1 [0:7];
    assign rq_1[0] = ($signed(inter_1[0]) > 127) ? 8'd127 : ($signed(inter_1[0]) < -128) ? -8'd128 : inter_1[0][7:0];
    assign rq_1[1] = ($signed(inter_1[1]) > 127) ? 8'd127 : ($signed(inter_1[1]) < -128) ? -8'd128 : inter_1[1][7:0];
    assign rq_1[2] = ($signed(inter_1[2]) > 127) ? 8'd127 : ($signed(inter_1[2]) < -128) ? -8'd128 : inter_1[2][7:0];
    assign rq_1[3] = ($signed(inter_1[3]) > 127) ? 8'd127 : ($signed(inter_1[3]) < -128) ? -8'd128 : inter_1[3][7:0];
    assign rq_1[4] = ($signed(inter_1[4]) > 127) ? 8'd127 : ($signed(inter_1[4]) < -128) ? -8'd128 : inter_1[4][7:0];
    assign rq_1[5] = ($signed(inter_1[5]) > 127) ? 8'd127 : ($signed(inter_1[5]) < -128) ? -8'd128 : inter_1[5][7:0];
    assign rq_1[6] = ($signed(inter_1[6]) > 127) ? 8'd127 : ($signed(inter_1[6]) < -128) ? -8'd128 : inter_1[6][7:0];
    assign rq_1[7] = ($signed(inter_1[7]) > 127) ? 8'd127 : ($signed(inter_1[7]) < -128) ? -8'd128 : inter_1[7][7:0];
    wire [15:0] inter_3 [0:7];
    wire v_3;
    wire [7:0] rq_2 [0:7];
    assign rq_2[0] = ($signed(inter_2[0]) > 127) ? 8'd127 : ($signed(inter_2[0]) < -128) ? -8'd128 : inter_2[0][7:0];
    assign rq_2[1] = ($signed(inter_2[1]) > 127) ? 8'd127 : ($signed(inter_2[1]) < -128) ? -8'd128 : inter_2[1][7:0];
    assign rq_2[2] = ($signed(inter_2[2]) > 127) ? 8'd127 : ($signed(inter_2[2]) < -128) ? -8'd128 : inter_2[2][7:0];
    assign rq_2[3] = ($signed(inter_2[3]) > 127) ? 8'd127 : ($signed(inter_2[3]) < -128) ? -8'd128 : inter_2[3][7:0];
    assign rq_2[4] = ($signed(inter_2[4]) > 127) ? 8'd127 : ($signed(inter_2[4]) < -128) ? -8'd128 : inter_2[4][7:0];
    assign rq_2[5] = ($signed(inter_2[5]) > 127) ? 8'd127 : ($signed(inter_2[5]) < -128) ? -8'd128 : inter_2[5][7:0];
    assign rq_2[6] = ($signed(inter_2[6]) > 127) ? 8'd127 : ($signed(inter_2[6]) < -128) ? -8'd128 : inter_2[6][7:0];
    assign rq_2[7] = ($signed(inter_2[7]) > 127) ? 8'd127 : ($signed(inter_2[7]) < -128) ? -8'd128 : inter_2[7][7:0];
    wire [15:0] inter_4 [0:7];
    wire v_4;
    wire [7:0] rq_3 [0:7];
    assign rq_3[0] = ($signed(inter_3[0]) > 127) ? 8'd127 : ($signed(inter_3[0]) < -128) ? -8'd128 : inter_3[0][7:0];
    assign rq_3[1] = ($signed(inter_3[1]) > 127) ? 8'd127 : ($signed(inter_3[1]) < -128) ? -8'd128 : inter_3[1][7:0];
    assign rq_3[2] = ($signed(inter_3[2]) > 127) ? 8'd127 : ($signed(inter_3[2]) < -128) ? -8'd128 : inter_3[2][7:0];
    assign rq_3[3] = ($signed(inter_3[3]) > 127) ? 8'd127 : ($signed(inter_3[3]) < -128) ? -8'd128 : inter_3[3][7:0];
    assign rq_3[4] = ($signed(inter_3[4]) > 127) ? 8'd127 : ($signed(inter_3[4]) < -128) ? -8'd128 : inter_3[4][7:0];
    assign rq_3[5] = ($signed(inter_3[5]) > 127) ? 8'd127 : ($signed(inter_3[5]) < -128) ? -8'd128 : inter_3[5][7:0];
    assign rq_3[6] = ($signed(inter_3[6]) > 127) ? 8'd127 : ($signed(inter_3[6]) < -128) ? -8'd128 : inter_3[6][7:0];
    assign rq_3[7] = ($signed(inter_3[7]) > 127) ? 8'd127 : ($signed(inter_3[7]) < -128) ? -8'd128 : inter_3[7][7:0];
    wire [15:0] inter_5 [0:7];
    wire v_5;
    wire [7:0] rq_4 [0:7];
    assign rq_4[0] = ($signed(inter_4[0]) > 127) ? 8'd127 : ($signed(inter_4[0]) < -128) ? -8'd128 : inter_4[0][7:0];
    assign rq_4[1] = ($signed(inter_4[1]) > 127) ? 8'd127 : ($signed(inter_4[1]) < -128) ? -8'd128 : inter_4[1][7:0];
    assign rq_4[2] = ($signed(inter_4[2]) > 127) ? 8'd127 : ($signed(inter_4[2]) < -128) ? -8'd128 : inter_4[2][7:0];
    assign rq_4[3] = ($signed(inter_4[3]) > 127) ? 8'd127 : ($signed(inter_4[3]) < -128) ? -8'd128 : inter_4[3][7:0];
    assign rq_4[4] = ($signed(inter_4[4]) > 127) ? 8'd127 : ($signed(inter_4[4]) < -128) ? -8'd128 : inter_4[4][7:0];
    assign rq_4[5] = ($signed(inter_4[5]) > 127) ? 8'd127 : ($signed(inter_4[5]) < -128) ? -8'd128 : inter_4[5][7:0];
    assign rq_4[6] = ($signed(inter_4[6]) > 127) ? 8'd127 : ($signed(inter_4[6]) < -128) ? -8'd128 : inter_4[6][7:0];
    assign rq_4[7] = ($signed(inter_4[7]) > 127) ? 8'd127 : ($signed(inter_4[7]) < -128) ? -8'd128 : inter_4[7][7:0];
    wire [15:0] inter_6 [0:7];
    wire v_6;
    wire [7:0] rq_5 [0:7];
    assign rq_5[0] = ($signed(inter_5[0]) > 127) ? 8'd127 : ($signed(inter_5[0]) < -128) ? -8'd128 : inter_5[0][7:0];
    assign rq_5[1] = ($signed(inter_5[1]) > 127) ? 8'd127 : ($signed(inter_5[1]) < -128) ? -8'd128 : inter_5[1][7:0];
    assign rq_5[2] = ($signed(inter_5[2]) > 127) ? 8'd127 : ($signed(inter_5[2]) < -128) ? -8'd128 : inter_5[2][7:0];
    assign rq_5[3] = ($signed(inter_5[3]) > 127) ? 8'd127 : ($signed(inter_5[3]) < -128) ? -8'd128 : inter_5[3][7:0];
    assign rq_5[4] = ($signed(inter_5[4]) > 127) ? 8'd127 : ($signed(inter_5[4]) < -128) ? -8'd128 : inter_5[4][7:0];
    assign rq_5[5] = ($signed(inter_5[5]) > 127) ? 8'd127 : ($signed(inter_5[5]) < -128) ? -8'd128 : inter_5[5][7:0];
    assign rq_5[6] = ($signed(inter_5[6]) > 127) ? 8'd127 : ($signed(inter_5[6]) < -128) ? -8'd128 : inter_5[6][7:0];
    assign rq_5[7] = ($signed(inter_5[7]) > 127) ? 8'd127 : ($signed(inter_5[7]) < -128) ? -8'd128 : inter_5[7][7:0];
    wire [15:0] inter_7 [0:7];
    wire v_7;
    wire [7:0] rq_6 [0:7];
    assign rq_6[0] = ($signed(inter_6[0]) > 127) ? 8'd127 : ($signed(inter_6[0]) < -128) ? -8'd128 : inter_6[0][7:0];
    assign rq_6[1] = ($signed(inter_6[1]) > 127) ? 8'd127 : ($signed(inter_6[1]) < -128) ? -8'd128 : inter_6[1][7:0];
    assign rq_6[2] = ($signed(inter_6[2]) > 127) ? 8'd127 : ($signed(inter_6[2]) < -128) ? -8'd128 : inter_6[2][7:0];
    assign rq_6[3] = ($signed(inter_6[3]) > 127) ? 8'd127 : ($signed(inter_6[3]) < -128) ? -8'd128 : inter_6[3][7:0];
    assign rq_6[4] = ($signed(inter_6[4]) > 127) ? 8'd127 : ($signed(inter_6[4]) < -128) ? -8'd128 : inter_6[4][7:0];
    assign rq_6[5] = ($signed(inter_6[5]) > 127) ? 8'd127 : ($signed(inter_6[5]) < -128) ? -8'd128 : inter_6[5][7:0];
    assign rq_6[6] = ($signed(inter_6[6]) > 127) ? 8'd127 : ($signed(inter_6[6]) < -128) ? -8'd128 : inter_6[6][7:0];
    assign rq_6[7] = ($signed(inter_6[7]) > 127) ? 8'd127 : ($signed(inter_6[7]) < -128) ? -8'd128 : inter_6[7][7:0];
    wire [15:0] inter_8 [0:7];
    wire v_8;
    wire [7:0] rq_7 [0:7];
    assign rq_7[0] = ($signed(inter_7[0]) > 127) ? 8'd127 : ($signed(inter_7[0]) < -128) ? -8'd128 : inter_7[0][7:0];
    assign rq_7[1] = ($signed(inter_7[1]) > 127) ? 8'd127 : ($signed(inter_7[1]) < -128) ? -8'd128 : inter_7[1][7:0];
    assign rq_7[2] = ($signed(inter_7[2]) > 127) ? 8'd127 : ($signed(inter_7[2]) < -128) ? -8'd128 : inter_7[2][7:0];
    assign rq_7[3] = ($signed(inter_7[3]) > 127) ? 8'd127 : ($signed(inter_7[3]) < -128) ? -8'd128 : inter_7[3][7:0];
    assign rq_7[4] = ($signed(inter_7[4]) > 127) ? 8'd127 : ($signed(inter_7[4]) < -128) ? -8'd128 : inter_7[4][7:0];
    assign rq_7[5] = ($signed(inter_7[5]) > 127) ? 8'd127 : ($signed(inter_7[5]) < -128) ? -8'd128 : inter_7[5][7:0];
    assign rq_7[6] = ($signed(inter_7[6]) > 127) ? 8'd127 : ($signed(inter_7[6]) < -128) ? -8'd128 : inter_7[6][7:0];
    assign rq_7[7] = ($signed(inter_7[7]) > 127) ? 8'd127 : ($signed(inter_7[7]) < -128) ? -8'd128 : inter_7[7][7:0];
    wire [15:0] inter_9 [0:7];
    wire v_9;
    wire [7:0] rq_8 [0:7];
    assign rq_8[0] = ($signed(inter_8[0]) > 127) ? 8'd127 : ($signed(inter_8[0]) < -128) ? -8'd128 : inter_8[0][7:0];
    assign rq_8[1] = ($signed(inter_8[1]) > 127) ? 8'd127 : ($signed(inter_8[1]) < -128) ? -8'd128 : inter_8[1][7:0];
    assign rq_8[2] = ($signed(inter_8[2]) > 127) ? 8'd127 : ($signed(inter_8[2]) < -128) ? -8'd128 : inter_8[2][7:0];
    assign rq_8[3] = ($signed(inter_8[3]) > 127) ? 8'd127 : ($signed(inter_8[3]) < -128) ? -8'd128 : inter_8[3][7:0];
    assign rq_8[4] = ($signed(inter_8[4]) > 127) ? 8'd127 : ($signed(inter_8[4]) < -128) ? -8'd128 : inter_8[4][7:0];
    assign rq_8[5] = ($signed(inter_8[5]) > 127) ? 8'd127 : ($signed(inter_8[5]) < -128) ? -8'd128 : inter_8[5][7:0];
    assign rq_8[6] = ($signed(inter_8[6]) > 127) ? 8'd127 : ($signed(inter_8[6]) < -128) ? -8'd128 : inter_8[6][7:0];
    assign rq_8[7] = ($signed(inter_8[7]) > 127) ? 8'd127 : ($signed(inter_8[7]) < -128) ? -8'd128 : inter_8[7][7:0];
    wire [7:0] rq_9 [0:7];
    assign rq_9[0] = ($signed(inter_9[0]) > 127) ? 8'd127 : ($signed(inter_9[0]) < -128) ? -8'd128 : inter_9[0][7:0];
    assign rq_9[1] = ($signed(inter_9[1]) > 127) ? 8'd127 : ($signed(inter_9[1]) < -128) ? -8'd128 : inter_9[1][7:0];
    assign rq_9[2] = ($signed(inter_9[2]) > 127) ? 8'd127 : ($signed(inter_9[2]) < -128) ? -8'd128 : inter_9[2][7:0];
    assign rq_9[3] = ($signed(inter_9[3]) > 127) ? 8'd127 : ($signed(inter_9[3]) < -128) ? -8'd128 : inter_9[3][7:0];
    assign rq_9[4] = ($signed(inter_9[4]) > 127) ? 8'd127 : ($signed(inter_9[4]) < -128) ? -8'd128 : inter_9[4][7:0];
    assign rq_9[5] = ($signed(inter_9[5]) > 127) ? 8'd127 : ($signed(inter_9[5]) < -128) ? -8'd128 : inter_9[5][7:0];
    assign rq_9[6] = ($signed(inter_9[6]) > 127) ? 8'd127 : ($signed(inter_9[6]) < -128) ? -8'd128 : inter_9[6][7:0];
    assign rq_9[7] = ($signed(inter_9[7]) > 127) ? 8'd127 : ($signed(inter_9[7]) < -128) ? -8'd128 : inter_9[7][7:0];

    bitnet_layer_0 layer_0 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(valid_in), .x_in(x_in),
        .valid_out(v_1), .y_out(inter_1),
        .relu_en(1'b1)
    );
    bitnet_layer_1 layer_1 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_1), .x_in(rq_1),
        .valid_out(v_2), .y_out(inter_2),
        .relu_en(1'b1)
    );
    bitnet_layer_2 layer_2 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_2), .x_in(rq_2),
        .valid_out(v_3), .y_out(inter_3),
        .relu_en(1'b1)
    );
    bitnet_layer_3 layer_3 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_3), .x_in(rq_3),
        .valid_out(v_4), .y_out(inter_4),
        .relu_en(1'b1)
    );
    bitnet_layer_4 layer_4 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_4), .x_in(rq_4),
        .valid_out(v_5), .y_out(inter_5),
        .relu_en(1'b1)
    );
    bitnet_layer_5 layer_5 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_5), .x_in(rq_5),
        .valid_out(v_6), .y_out(inter_6),
        .relu_en(1'b1)
    );
    bitnet_layer_6 layer_6 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_6), .x_in(rq_6),
        .valid_out(v_7), .y_out(inter_7),
        .relu_en(1'b1)
    );
    bitnet_layer_7 layer_7 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_7), .x_in(rq_7),
        .valid_out(v_8), .y_out(inter_8),
        .relu_en(1'b1)
    );
    bitnet_layer_8 layer_8 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_8), .x_in(rq_8),
        .valid_out(v_9), .y_out(inter_9),
        .relu_en(1'b1)
    );
    bitnet_layer_9 layer_9 (
        .clk(clk), .rst_n(rst_n),
        .valid_in(v_9), .x_in(rq_9),
        .valid_out(valid_out), .y_out(y_out),
        .relu_en(1'b0)
    );

endmodule
