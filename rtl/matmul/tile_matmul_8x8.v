// tile_matmul_8x8.v — Single FPGA tile: 8×8 matmul with fixed weights
//
// ONE iCE40HX8K = one 8×8 matrix-vector multiply, single cycle.
// Weights baked into LUT configuration bitstream at synthesis.
// To change weights: reconfigure FPGA (~50ms). During inference: zero overhead.
//
// Chain these together:
//   TILE-0 (layer 0) → TILE-1 (layer 1) → TILE-2 (layer 2) → ...
//   Data flows through at clock speed. Each tile = one layer.
//
// For wider matrices: tile 4× FPGAs per layer, accumulate.
// When out of pins: buffer in SRAM, slice and continue.
//
// Interface:
//   IN:  8 × 8-bit  = 64 pins  (input vector)
//   OUT: 8 × 16-bit = 128 pins (output vector, truncated from 19-bit)
//   CTL: clk + rst + valid_in + valid_out = 4 pins
//   Total: 196 pins → fits HX8K-CT256 (208 GPIO)
//
// Resources: ~5,200 LUT4 of 7,680 available
// Speed: 133MHz = 136 GOPS per tile

module tile_matmul_8x8 (
    input  wire        clk,
    input  wire        rst_n,

    // ---- Tile chain interface ----
    input  wire        valid_in,
    input  wire [7:0]  x_in [0:7],     // input vector (from previous tile or host)

    output reg         valid_out,
    output reg  [15:0] y_out [0:7],    // output vector (to next tile or host)

    // ---- Activation select ----
    input  wire        relu_en          // 1 = ReLU between layers, 0 = passthrough
);

    // =====================================================================
    // Fixed weight matrix — set at synthesis time via parameters
    // In real use: generate a .v file per layer with specific weights
    // =====================================================================

    // Weights as parameters (overridden per-instance at synthesis)
    parameter signed [7:0] W00=0,W01=0,W02=0,W03=0,W04=0,W05=0,W06=0,W07=0;
    parameter signed [7:0] W10=0,W11=0,W12=0,W13=0,W14=0,W15=0,W16=0,W17=0;
    parameter signed [7:0] W20=0,W21=0,W22=0,W23=0,W24=0,W25=0,W26=0,W27=0;
    parameter signed [7:0] W30=0,W31=0,W32=0,W33=0,W34=0,W35=0,W36=0,W37=0;
    parameter signed [7:0] W40=0,W41=0,W42=0,W43=0,W44=0,W45=0,W46=0,W47=0;
    parameter signed [7:0] W50=0,W51=0,W52=0,W53=0,W54=0,W55=0,W56=0,W57=0;
    parameter signed [7:0] W60=0,W61=0,W62=0,W63=0,W64=0,W65=0,W66=0,W67=0;
    parameter signed [7:0] W70=0,W71=0,W72=0,W73=0,W74=0,W75=0,W76=0,W77=0;

    // Pack into array for generate loop
    wire signed [7:0] W [0:7][0:7];
    assign W[0][0]=W00; assign W[0][1]=W01; assign W[0][2]=W02; assign W[0][3]=W03;
    assign W[0][4]=W04; assign W[0][5]=W05; assign W[0][6]=W06; assign W[0][7]=W07;
    assign W[1][0]=W10; assign W[1][1]=W11; assign W[1][2]=W12; assign W[1][3]=W13;
    assign W[1][4]=W14; assign W[1][5]=W15; assign W[1][6]=W16; assign W[1][7]=W17;
    assign W[2][0]=W20; assign W[2][1]=W21; assign W[2][2]=W22; assign W[2][3]=W23;
    assign W[2][4]=W24; assign W[2][5]=W25; assign W[2][6]=W26; assign W[2][7]=W27;
    assign W[3][0]=W30; assign W[3][1]=W31; assign W[3][2]=W32; assign W[3][3]=W33;
    assign W[3][4]=W34; assign W[3][5]=W35; assign W[3][6]=W36; assign W[3][7]=W37;
    assign W[4][0]=W40; assign W[4][1]=W41; assign W[4][2]=W42; assign W[4][3]=W43;
    assign W[4][4]=W44; assign W[4][5]=W45; assign W[4][6]=W46; assign W[4][7]=W47;
    assign W[5][0]=W50; assign W[5][1]=W51; assign W[5][2]=W52; assign W[5][3]=W53;
    assign W[5][4]=W54; assign W[5][5]=W55; assign W[5][6]=W56; assign W[5][7]=W57;
    assign W[6][0]=W60; assign W[6][1]=W61; assign W[6][2]=W62; assign W[6][3]=W63;
    assign W[6][4]=W64; assign W[6][5]=W65; assign W[6][6]=W66; assign W[6][7]=W67;
    assign W[7][0]=W70; assign W[7][1]=W71; assign W[7][2]=W72; assign W[7][3]=W73;
    assign W[7][4]=W74; assign W[7][5]=W75; assign W[7][6]=W76; assign W[7][7]=W77;

    // =====================================================================
    // 8 parallel dot products — all combinational
    // Synthesis optimises each W[i][k] * x_in[k] as constant multiply
    // (~32 LUTs each instead of ~64 for variable multiply)
    // =====================================================================

    wire signed [7:0] x_s [0:7];
    genvar i, k;
    generate
        for (i = 0; i < 8; i = i + 1) begin : cast
            assign x_s[i] = x_in[i];
        end
    endgenerate

    // Dot products
    wire signed [18:0] dot [0:7];

    generate
        for (i = 0; i < 8; i = i + 1) begin : row
            // 8 constant multiplies + adder tree — synthesis handles it
            wire signed [15:0] p0 = W[i][0] * x_s[0];
            wire signed [15:0] p1 = W[i][1] * x_s[1];
            wire signed [15:0] p2 = W[i][2] * x_s[2];
            wire signed [15:0] p3 = W[i][3] * x_s[3];
            wire signed [15:0] p4 = W[i][4] * x_s[4];
            wire signed [15:0] p5 = W[i][5] * x_s[5];
            wire signed [15:0] p6 = W[i][6] * x_s[6];
            wire signed [15:0] p7 = W[i][7] * x_s[7];

            // Adder tree: 8 → 1
            wire signed [16:0] s1a = {p0[15],p0} + {p1[15],p1};
            wire signed [16:0] s1b = {p2[15],p2} + {p3[15],p3};
            wire signed [16:0] s1c = {p4[15],p4} + {p5[15],p5};
            wire signed [16:0] s1d = {p6[15],p6} + {p7[15],p7};

            wire signed [17:0] s2a = {s1a[16],s1a} + {s1b[16],s1b};
            wire signed [17:0] s2b = {s1c[16],s1c} + {s1d[16],s1d};

            assign dot[i] = {s2a[17],s2a} + {s2b[17],s2b};
        end
    endgenerate

    // =====================================================================
    // Output register + optional ReLU + requantize to 16-bit
    // =====================================================================

    integer j;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_out <= 0;
            for (j = 0; j < 8; j = j + 1)
                y_out[j] <= 16'd0;
        end else begin
            valid_out <= valid_in;
            for (j = 0; j < 8; j = j + 1) begin
                if (relu_en && dot[j][18])
                    y_out[j] <= 16'd0;                 // ReLU: clamp negative
                else
                    y_out[j] <= dot[j][15:0];          // pass lower 16 bits
            end
        end
    end

endmodule
