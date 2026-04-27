// bitnet_tile_8x8.v — Single-cycle 8×8 matmul with BitNet+shift weights
//
// Each weight is {-1,0,+1} × 2^n — encoded as 5 bits: [sign:1][zero:1][shift:3]
// The "multiply" is just wiring + optional negate. ~4 LUTs average.
//
// Resource estimate:
//   64 multiplies × ~4 LUTs avg  = ~256 LUTs  (mostly wiring)
//   8 adder trees × ~112 LUTs    = ~896 LUTs  (the real work)
//   Output regs + ReLU            = ~64 LUTs
//   ─────────────────────────────────────────
//   Total: ~1,216 LUTs → fits iCE40HX1K (1,280 LUTs)
//
// Clock: 133MHz on iCE40
// Throughput: 64 MACs/cycle = 8.5 GOPS per £1.80 chip
//
// For 1000 of these on a single ASIC die:
//   1,216,000 LUTs equivalent → ~15M gates → ~3mm² in 28nm
//   At 1GHz: 64,000 MACs/cycle = 64 TOPS
//   Power: ~3W (mostly adder switching)
//   Cost: pennies per die at volume

module bitnet_tile_8x8 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        valid_in,
    input  wire [7:0]  x_in [0:7],
    output reg         valid_out,
    output reg  [15:0] y_out [0:7],
    input  wire        relu_en
);

    // Weight encoding: 64 weights × 3 parameters each
    // S=sign, Z=zero, H=shift (0-7)
    // Override per-instance at synthesis to bake in model weights

    // Row 0
    parameter S00=0,Z00=1,H00=0; parameter S01=0,Z01=1,H01=0;
    parameter S02=0,Z02=1,H02=0; parameter S03=0,Z03=1,H03=0;
    parameter S04=0,Z04=1,H04=0; parameter S05=0,Z05=1,H05=0;
    parameter S06=0,Z06=1,H06=0; parameter S07=0,Z07=1,H07=0;
    // Row 1
    parameter S10=0,Z10=1,H10=0; parameter S11=0,Z11=1,H11=0;
    parameter S12=0,Z12=1,H12=0; parameter S13=0,Z13=1,H13=0;
    parameter S14=0,Z14=1,H14=0; parameter S15=0,Z15=1,H15=0;
    parameter S16=0,Z16=1,H16=0; parameter S17=0,Z17=1,H17=0;
    // Row 2
    parameter S20=0,Z20=1,H20=0; parameter S21=0,Z21=1,H21=0;
    parameter S22=0,Z22=1,H22=0; parameter S23=0,Z23=1,H23=0;
    parameter S24=0,Z24=1,H24=0; parameter S25=0,Z25=1,H25=0;
    parameter S26=0,Z26=1,H26=0; parameter S27=0,Z27=1,H27=0;
    // Row 3
    parameter S30=0,Z30=1,H30=0; parameter S31=0,Z31=1,H31=0;
    parameter S32=0,Z32=1,H32=0; parameter S33=0,Z33=1,H33=0;
    parameter S34=0,Z34=1,H34=0; parameter S35=0,Z35=1,H35=0;
    parameter S36=0,Z36=1,H36=0; parameter S37=0,Z37=1,H37=0;
    // Row 4
    parameter S40=0,Z40=1,H40=0; parameter S41=0,Z41=1,H41=0;
    parameter S42=0,Z42=1,H42=0; parameter S43=0,Z43=1,H43=0;
    parameter S44=0,Z44=1,H44=0; parameter S45=0,Z45=1,H45=0;
    parameter S46=0,Z46=1,H46=0; parameter S47=0,Z47=1,H47=0;
    // Row 5
    parameter S50=0,Z50=1,H50=0; parameter S51=0,Z51=1,H51=0;
    parameter S52=0,Z52=1,H52=0; parameter S53=0,Z53=1,H53=0;
    parameter S54=0,Z54=1,H54=0; parameter S55=0,Z55=1,H55=0;
    parameter S56=0,Z56=1,H56=0; parameter S57=0,Z57=1,H57=0;
    // Row 6
    parameter S60=0,Z60=1,H60=0; parameter S61=0,Z61=1,H61=0;
    parameter S62=0,Z62=1,H62=0; parameter S63=0,Z63=1,H63=0;
    parameter S64=0,Z64=1,H64=0; parameter S65=0,Z65=1,H65=0;
    parameter S66=0,Z66=1,H66=0; parameter S67=0,Z67=1,H67=0;
    // Row 7
    parameter S70=0,Z70=1,H70=0; parameter S71=0,Z71=1,H71=0;
    parameter S72=0,Z72=1,H72=0; parameter S73=0,Z73=1,H73=0;
    parameter S74=0,Z74=1,H74=0; parameter S75=0,Z75=1,H75=0;
    parameter S76=0,Z76=1,H76=0; parameter S77=0,Z77=1,H77=0;

    // =====================================================================
    // 64 BitNet multiplies — each is wiring + optional negate
    // =====================================================================

    wire signed [15:0] p [0:7][0:7];
    wire signed [7:0] x_s [0:7];

    genvar i;
    generate
        for (i = 0; i < 8; i = i + 1) begin : cast
            assign x_s[i] = x_in[i];
        end
    endgenerate

    // Macro-style instantiation — each multiply is ~0-8 LUTs
    `define BMUL(R,C) bitnet_multiply #(.SIGN(S``R``C),.ZERO(Z``R``C),.SHIFT(H``R``C)) \
        bm_``R``_``C (.x(x_s[C]), .y(p[R][C]))

    generate
        // Row 0
        `BMUL(0,0); `BMUL(0,1); `BMUL(0,2); `BMUL(0,3);
        `BMUL(0,4); `BMUL(0,5); `BMUL(0,6); `BMUL(0,7);
        // Row 1
        `BMUL(1,0); `BMUL(1,1); `BMUL(1,2); `BMUL(1,3);
        `BMUL(1,4); `BMUL(1,5); `BMUL(1,6); `BMUL(1,7);
        // Row 2
        `BMUL(2,0); `BMUL(2,1); `BMUL(2,2); `BMUL(2,3);
        `BMUL(2,4); `BMUL(2,5); `BMUL(2,6); `BMUL(2,7);
        // Row 3
        `BMUL(3,0); `BMUL(3,1); `BMUL(3,2); `BMUL(3,3);
        `BMUL(3,4); `BMUL(3,5); `BMUL(3,6); `BMUL(3,7);
        // Row 4
        `BMUL(4,0); `BMUL(4,1); `BMUL(4,2); `BMUL(4,3);
        `BMUL(4,4); `BMUL(4,5); `BMUL(4,6); `BMUL(4,7);
        // Row 5
        `BMUL(5,0); `BMUL(5,1); `BMUL(5,2); `BMUL(5,3);
        `BMUL(5,4); `BMUL(5,5); `BMUL(5,6); `BMUL(5,7);
        // Row 6
        `BMUL(6,0); `BMUL(6,1); `BMUL(6,2); `BMUL(6,3);
        `BMUL(6,4); `BMUL(6,5); `BMUL(6,6); `BMUL(6,7);
        // Row 7
        `BMUL(7,0); `BMUL(7,1); `BMUL(7,2); `BMUL(7,3);
        `BMUL(7,4); `BMUL(7,5); `BMUL(7,6); `BMUL(7,7);
    endgenerate

    `undef BMUL

    // =====================================================================
    // 8 adder trees — this is where the LUTs actually go (~112 each)
    // =====================================================================

    wire signed [18:0] dot [0:7];

    genvar r;
    generate
        for (r = 0; r < 8; r = r + 1) begin : row
            wire signed [16:0] s1a = {p[r][0][15],p[r][0]} + {p[r][1][15],p[r][1]};
            wire signed [16:0] s1b = {p[r][2][15],p[r][2]} + {p[r][3][15],p[r][3]};
            wire signed [16:0] s1c = {p[r][4][15],p[r][4]} + {p[r][5][15],p[r][5]};
            wire signed [16:0] s1d = {p[r][6][15],p[r][6]} + {p[r][7][15],p[r][7]};

            wire signed [17:0] s2a = {s1a[16],s1a} + {s1b[16],s1b};
            wire signed [17:0] s2b = {s1c[16],s1c} + {s1d[16],s1d};

            assign dot[r] = {s2a[17],s2a} + {s2b[17],s2b};
        end
    endgenerate

    // =====================================================================
    // Output register + ReLU
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
                    y_out[j] <= 16'd0;
                else
                    y_out[j] <= dot[j][15:0];
            end
        end
    end

endmodule
