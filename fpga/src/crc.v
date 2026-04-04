`timescale 1ns / 1ps
//============================================================================
// crc.v — CRC7 and CRC16 calculators for SD card protocol
//
// Both modules process one serial bit per clock when enable is high.
// Assert clear to reset the CRC register to zero before a new calculation.
//============================================================================

//----------------------------------------------------------------------------
// crc7 — SD command CRC
//   Polynomial: x^7 + x^3 + 1  (0x09)
//   Generator: G(x) = 1_0001_001  (bit 7 implicit)
//----------------------------------------------------------------------------
module crc7 (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       clear,      // synchronous clear (reset CRC to 0)
    input  wire       enable,     // process data_in this cycle
    input  wire       data_in,    // serial data bit (MSB first)
    output wire [6:0] crc_out     // current CRC value
);

    reg [6:0] crc;
    assign crc_out = crc;

    // XOR feedback bit: MSB of register XOR incoming data
    wire fb = crc[6] ^ data_in;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            crc <= 7'd0;
        end else if (clear) begin
            crc <= 7'd0;
        end else if (enable) begin
            // Shift left by 1, XOR feedback into tap positions.
            // Polynomial 0x09 → taps at bit 3 and bit 0.
            //   new[6] = old[5]
            //   new[5] = old[4]
            //   new[4] = old[3]
            //   new[3] = old[2] ^ fb   (x^3 tap)
            //   new[2] = old[1]
            //   new[1] = old[0]
            //   new[0] = fb             (x^0 tap)
            crc[6] <= crc[5];
            crc[5] <= crc[4];
            crc[4] <= crc[3];
            crc[3] <= crc[2] ^ fb;
            crc[2] <= crc[1];
            crc[1] <= crc[0];
            crc[0] <= fb;
        end
    end

endmodule

//----------------------------------------------------------------------------
// crc16 — SD data CRC (CRC-16-CCITT)
//   Polynomial: x^16 + x^12 + x^5 + 1  (0x1021)
//   Generator: G(x) = 1_0001_0000_0010_0001  (bit 16 implicit)
//----------------------------------------------------------------------------
module crc16 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clear,      // synchronous clear (reset CRC to 0)
    input  wire        enable,     // process data_in this cycle
    input  wire        data_in,    // serial data bit (MSB first)
    output wire [15:0] crc_out     // current CRC value
);

    reg [15:0] crc;
    assign crc_out = crc;

    // XOR feedback bit: MSB of register XOR incoming data
    wire fb = crc[15] ^ data_in;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            crc <= 16'd0;
        end else if (clear) begin
            crc <= 16'd0;
        end else if (enable) begin
            // Shift left by 1, XOR feedback into tap positions.
            // Polynomial 0x1021 → taps at bit 12, bit 5, and bit 0.
            //   new[15] = old[14]
            //   new[14] = old[13]
            //   new[13] = old[12]
            //   new[12] = old[11] ^ fb  (x^12 tap)
            //   new[11] = old[10]
            //   new[10] = old[9]
            //   new[9]  = old[8]
            //   new[8]  = old[7]
            //   new[7]  = old[6]
            //   new[6]  = old[5]
            //   new[5]  = old[4]  ^ fb  (x^5 tap)
            //   new[4]  = old[3]
            //   new[3]  = old[2]
            //   new[2]  = old[1]
            //   new[1]  = old[0]
            //   new[0]  = fb            (x^0 tap)
            crc[15] <= crc[14];
            crc[14] <= crc[13];
            crc[13] <= crc[12];
            crc[12] <= crc[11] ^ fb;
            crc[11] <= crc[10];
            crc[10] <= crc[9];
            crc[9]  <= crc[8];
            crc[8]  <= crc[7];
            crc[7]  <= crc[6];
            crc[6]  <= crc[5];
            crc[5]  <= crc[4]  ^ fb;
            crc[4]  <= crc[3];
            crc[3]  <= crc[2];
            crc[2]  <= crc[1];
            crc[1]  <= crc[0];
            crc[0]  <= fb;
        end
    end

endmodule
