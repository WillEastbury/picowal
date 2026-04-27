// adder_tree_8.v — Reduce 8 × 16-bit signed products to one 19-bit sum
// 3-stage pipelined: result valid 3 cycles after input
// Pure LUT logic — no BRAM, no DSP.
//
// Resource: ~120 LUTs on iCE40HX (carry-chain adders)

module adder_tree_8 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        in_valid,

    input  wire [15:0] p0, p1, p2, p3, p4, p5, p6, p7,

    output reg  [18:0] sum_out,
    output reg         out_valid
);

    // All values are signed (products of signed INT8 × signed INT8)
    wire signed [15:0] sp0=p0, sp1=p1, sp2=p2, sp3=p3,
                       sp4=p4, sp5=p5, sp6=p6, sp7=p7;

    // --- Stage 1: 8 → 4 (pairs) ---
    reg signed [16:0] s1_0, s1_1, s1_2, s1_3;
    reg               s1_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1_valid <= 0;
        end else begin
            s1_0 <= {sp0[15], sp0} + {sp1[15], sp1};
            s1_1 <= {sp2[15], sp2} + {sp3[15], sp3};
            s1_2 <= {sp4[15], sp4} + {sp5[15], sp5};
            s1_3 <= {sp6[15], sp6} + {sp7[15], sp7};
            s1_valid <= in_valid;
        end
    end

    // --- Stage 2: 4 → 2 ---
    reg signed [17:0] s2_0, s2_1;
    reg               s2_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s2_valid <= 0;
        end else begin
            s2_0 <= {s1_0[16], s1_0} + {s1_1[16], s1_1};
            s2_1 <= {s1_2[16], s1_2} + {s1_3[16], s1_3};
            s2_valid <= s1_valid;
        end
    end

    // --- Stage 3: 2 → 1 ---
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum_out   <= 0;
            out_valid <= 0;
        end else begin
            sum_out   <= {s2_0[17], s2_0} + {s2_1[17], s2_1};
            out_valid <= s2_valid;
        end
    end

endmodule
