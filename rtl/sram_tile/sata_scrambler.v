// SATA Scrambler / Descrambler
// LFSR polynomial: G(x) = x^16 + x^15 + x^13 + x^4 + 1
// Galois LFSR form, initial seed: 0xFFFF
// Processes one 32-bit DWORD per clock cycle
// Same module for scramble and descramble (XOR is self-inverse)

module sata_scrambler (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        init,        // pulse to reset LFSR to 0xFFFF
    input  wire [31:0] data_in,
    input  wire        valid,
    output wire [31:0] data_out     // XOR of data_in with LFSR stream
);

    localparam [15:0] LFSR_SEED = 16'hFFFF;

    // Galois LFSR taps for x^16 + x^15 + x^13 + x^4 + 1:
    // XOR at bit positions 15, 13, 4 when output (bit 15) is 1
    localparam [15:0] LFSR_TAPS = (16'h1 << 15) | (16'h1 << 13) | (16'h1 << 4);

    reg [15:0] lfsr;

    // Advance LFSR by 32 serial steps, producing 32 scramble bits
    // Returns {next_lfsr[15:0], scramble_bits[31:0]}
    function [47:0] advance_lfsr;
        input [15:0] state;
        reg [15:0] s;
        reg [31:0] bits;
        reg        out_bit;
        integer    i;
        begin
            s = state;
            for (i = 0; i < 32; i = i + 1) begin
                out_bit = s[15];
                bits[i] = out_bit;
                s = {s[14:0], out_bit};
                if (out_bit)
                    s = s ^ LFSR_TAPS;
            end
            advance_lfsr = {s, bits};
        end
    endfunction

    wire [47:0] lfsr_result = advance_lfsr(lfsr);
    wire [15:0] next_lfsr   = lfsr_result[47:32];
    wire [31:0] scramble_bits = lfsr_result[31:0];

    assign data_out = data_in ^ scramble_bits;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            lfsr <= LFSR_SEED;
        else if (init)
            lfsr <= LFSR_SEED;
        else if (valid)
            lfsr <= next_lfsr;
    end

endmodule
