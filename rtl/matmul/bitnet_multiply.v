// bitnet_multiply.v — BitNet+shift multiply: {-1,0,+1} × 2^n
//
// Weight encoding: [sign:1][zero:1][shift:3] = 5 bits per weight
//   zero=1:          output = 0                  (0 LUTs — tied low)
//   zero=0, sign=0:  output = x << shift         (0 LUTs — just wiring)
//   zero=0, sign=1:  output = -(x << shift)      (~8 LUTs — invert + carry)
//
// The "multiply" is a WIRING DECISION. Synthesis resolves it to gates or nothing.
// Average cost: ~4 LUT4 per multiply.

module bitnet_multiply #(
    parameter SIGN  = 0,     // 0 = positive, 1 = negative
    parameter ZERO  = 0,     // 1 = weight is zero (output 0)
    parameter SHIFT = 0      // 0-7: left shift amount (power of 2 scale)
)(
    input  wire signed [7:0]  x,
    output wire signed [15:0] y
);

    generate
        if (ZERO) begin : zero_weight
            assign y = 16'sd0;
        end else if (SIGN == 0 && SHIFT == 0) begin : pos_unity
            assign y = {{8{x[7]}}, x};                        // just sign-extend
        end else if (SIGN == 0) begin : pos_shift
            assign y = {{(8-SHIFT){x[7]}}, x, {SHIFT{1'b0}}}; // x << SHIFT
        end else if (SHIFT == 0) begin : neg_unity
            assign y = -{{8{x[7]}}, x};                       // negate
        end else begin : neg_shift
            assign y = -({{(8-SHIFT){x[7]}}, x, {SHIFT{1'b0}}}); // -(x << SHIFT)
        end
    endgenerate

endmodule
