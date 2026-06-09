`timescale 1ns/1ps
module ui_rom (
    input  wire        clk,
    input  wire [11:0] addr,    // 12-bit = 4096 bytes max
    output reg  [7:0]  data_out,
    output wire [11:0] page_len  // actual length of stored HTML
);

    localparam PAGE_LEN = 12'd403;

    assign page_len = PAGE_LEN;

    reg [7:0] rom [0:4095];

    initial $readmemh("src/ui_rom.hex", rom);

    always @(posedge clk)
        data_out <= rom[addr];

endmodule
