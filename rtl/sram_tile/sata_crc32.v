// SATA CRC32 Engine
// Polynomial: 0x04C11DB7 (standard CRC-32)
// Initial value: 0x52325032 (SATA-specific)
// Processes one 32-bit DWORD per clock cycle (LSB first)

module sata_crc32 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        init,        // pulse to reset CRC to 0x52325032
    input  wire [31:0] data_in,
    input  wire        valid,
    output wire [31:0] crc_out      // current CRC value
);

    localparam [31:0] CRC_INIT = 32'h52325032;
    localparam [31:0] POLY     = 32'h04C11DB7;

    reg [31:0] crc_reg;

    // Compute next CRC over 32 data bits (LSB first, per SATA spec)
    function [31:0] next_crc;
        input [31:0] crc;
        input [31:0] data;
        reg [31:0] c;
        reg        fb;
        integer    i;
        begin
            c = crc;
            for (i = 0; i < 32; i = i + 1) begin
                fb = c[31] ^ data[i];
                c  = {c[30:0], 1'b0} ^ ({32{fb}} & POLY);
            end
            next_crc = c;
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            crc_reg <= CRC_INIT;
        else if (init)
            crc_reg <= CRC_INIT;
        else if (valid)
            crc_reg <= next_crc(crc_reg, data_in);
    end

    assign crc_out = crc_reg;

endmodule
