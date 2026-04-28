// eth_crc32.v — Ethernet CRC32 (FCS) Generator/Checker
//
// Standard Ethernet CRC32: polynomial 0x04C11DB7
// Processes 8 bits per clock cycle
// Init: 0xFFFFFFFF, final XOR: 0xFFFFFFFF, bit-reversed output

module eth_crc32 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        init,        // reset CRC to 0xFFFFFFFF
    input  wire [7:0]  data_in,
    input  wire        valid,
    output wire [31:0] crc_out,     // current CRC (apply final XOR externally)
    output wire [31:0] fcs_out      // bit-reversed, inverted — ready to append
);

    reg [31:0] crc;

    // CRC32 8-bit parallel XOR matrix
    // Polynomial 0x04C11DB7, reflected (LSB-first processing)
    function [31:0] crc_next;
        input [31:0] c;
        input [7:0]  d;
        reg [31:0] n;
        integer i;
        begin
            n = c;
            for (i = 0; i < 8; i = i + 1) begin
                if (n[0] ^ d[i])
                    n = (n >> 1) ^ 32'hEDB88320;
                else
                    n = n >> 1;
            end
            crc_next = n;
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            crc <= 32'hFFFFFFFF;
        else if (init)
            crc <= 32'hFFFFFFFF;
        else if (valid)
            crc <= crc_next(crc, data_in);
    end

    assign crc_out = crc;

    // FCS = bit-reverse each byte of ~CRC
    wire [31:0] crc_inv = ~crc;
    assign fcs_out = {
        crc_inv[24], crc_inv[25], crc_inv[26], crc_inv[27],
        crc_inv[28], crc_inv[29], crc_inv[30], crc_inv[31],
        crc_inv[16], crc_inv[17], crc_inv[18], crc_inv[19],
        crc_inv[20], crc_inv[21], crc_inv[22], crc_inv[23],
        crc_inv[8],  crc_inv[9],  crc_inv[10], crc_inv[11],
        crc_inv[12], crc_inv[13], crc_inv[14], crc_inv[15],
        crc_inv[0],  crc_inv[1],  crc_inv[2],  crc_inv[3],
        crc_inv[4],  crc_inv[5],  crc_inv[6],  crc_inv[7]
    };

endmodule


// ip_checksum.v — IPv4 Header Checksum Calculator
//
// Ones-complement sum of 16-bit words in the IP header (20 bytes = 10 words)
// Set checksum field to 0 before computing

module ip_checksum (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        init,
    input  wire [15:0] word_in,     // feed 10 header words sequentially
    input  wire        valid,
    input  wire        finish,      // pulse after last word
    output reg  [15:0] checksum,    // valid after finish
    output reg         done
);

    reg [31:0] sum;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum      <= 32'd0;
            checksum <= 16'd0;
            done     <= 1'b0;
        end else begin
            done <= 1'b0;

            if (init) begin
                sum  <= 32'd0;
                done <= 1'b0;
            end else if (valid) begin
                sum <= sum + {16'd0, word_in};
            end else if (finish) begin
                // Fold 32-bit to 16-bit with carry
                checksum <= ~(sum[15:0] + sum[31:16]);
                done     <= 1'b1;
            end
        end
    end

endmodule
