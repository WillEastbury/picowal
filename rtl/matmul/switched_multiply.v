// switched_multiply.v — Single INT8 "multiply" via BRAM lookup
// No arithmetic gates. Weight is precomputed into a 256-entry ROM.
// Input byte is the address. Product appears in one cycle.
//
// Target: iCE40HX series (SB_RAM256x16 primitive)
// Resource: 1 BRAM block per multiply. Zero LUTs.

module switched_multiply (
    input  wire        clk,
    input  wire [7:0]  x_in,       // input activation (address)
    input  wire [7:0]  w_addr,     // weight table write address
    input  wire [15:0] w_data,     // weight table write data (precomputed product)
    input  wire        w_we,       // weight table write enable
    output wire [15:0] product     // switched product, 1-cycle latency
);

    // iCE40 BRAM primitive: 256 entries × 16 bits = 4Kbit = 1 block
    // Read port: addressed by x_in (the activation byte)
    // Write port: used at load time to fill the lookup table
    //
    // At model load:  for x in 0..255: write(x, weight * (x as signed int8))
    // At inference:   product = table[x_in]  — one cycle, zero gates

    SB_RAM256x16 weight_rom (
        .RDATA  (product),
        .RADDR  (x_in),
        .RCLK   (clk),
        .RCLKE  (1'b1),
        .RE     (1'b1),

        .WDATA  (w_data),
        .WADDR  (w_addr),
        .WCLK   (clk),
        .WCLKE  (1'b1),
        .WE     (w_we)
    );

endmodule
