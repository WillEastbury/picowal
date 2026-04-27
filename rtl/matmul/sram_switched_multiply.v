// sram_switched_multiply.v — External parallel SRAM as lookup-table multiplier
//
// The SRAM chip IS the compute element. No arithmetic in the FPGA.
//
// Target: IS61WV25616BLL (256K × 16-bit, 10ns async SRAM, TSOP-44, ~$2)
//   - 18 address pins: [17:8] = weight select (1024 weights), [7:0] = input value
//   - 16 data pins: precomputed INT8 × INT8 product (signed 16-bit)
//   - Active-low CE#, OE#, WE#
//
// At model load: host writes table[weight_idx][x] = weight × (signed)x for all 256 x values
// At inference:  present weight_idx on addr[17:8], input byte on addr[7:0]
//                → product appears on data pins in 10ns. No clock needed.
//
// FPGA role: address routing + control signals only. Zero compute.

module sram_switched_multiply (
    input  wire        clk,
    input  wire        rst_n,

    // ---- Control ----
    input  wire        mode_write,     // 1 = loading weights, 0 = inference
    input  wire        read_valid,     // pulse: initiate async read

    // ---- Inference path ----
    input  wire [9:0]  weight_sel,     // which of 1024 weight tables
    input  wire [7:0]  x_in,          // input activation byte (address)

    // ---- Weight loading path ----
    input  wire [17:0] wr_addr,       // full 18-bit write address
    input  wire [15:0] wr_data,       // product to store
    input  wire        wr_valid,      // write strobe

    // ---- Physical SRAM interface (directly to chip pins) ----
    output reg  [17:0] sram_addr,     // A[17:0]
    inout  wire [15:0] sram_data,     // D[15:0] bidirectional
    output reg         sram_ce_n,     // chip enable (active low)
    output reg         sram_oe_n,     // output enable (active low)
    output reg         sram_we_n,     // write enable (active low)

    // ---- Result ----
    output reg  [15:0] product,       // latched read result
    output reg         product_valid
);

    // Bidirectional data bus
    reg [15:0] sram_data_out;
    reg        sram_data_oe;
    assign sram_data = sram_data_oe ? sram_data_out : 16'hZZZZ;

    // Async read timing: addr stable → data valid in 10ns
    // At 133MHz (7.5ns period), need 2 clock cycles for safe capture
    reg [1:0] read_pipe;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sram_ce_n     <= 1;
            sram_oe_n     <= 1;
            sram_we_n     <= 1;
            sram_data_oe  <= 0;
            product_valid <= 0;
            read_pipe     <= 0;
        end else if (mode_write && wr_valid) begin
            // --- Write cycle: precompute weight table ---
            sram_addr    <= wr_addr;
            sram_data_out <= wr_data;
            sram_data_oe <= 1;
            sram_ce_n    <= 0;
            sram_oe_n    <= 1;
            sram_we_n    <= 0;      // WE# active
            product_valid <= 0;
            read_pipe    <= 0;
        end else if (!mode_write && read_valid) begin
            // --- Read cycle: async lookup ---
            sram_addr    <= {weight_sel, x_in};
            sram_data_oe <= 0;       // release data bus for SRAM to drive
            sram_ce_n    <= 0;
            sram_oe_n    <= 0;       // OE# active
            sram_we_n    <= 1;
            product_valid <= 0;
            read_pipe    <= 2'b01;   // start 2-cycle capture pipeline
        end else begin
            // Pipeline: wait for async access time then capture
            read_pipe <= {read_pipe[0], 1'b0};
            if (read_pipe[1]) begin
                product       <= sram_data;  // capture after 2 clocks (~15ns > 10ns tAA)
                product_valid <= 1;
            end else begin
                product_valid <= 0;
            end
            // Deassert controls when idle
            if (read_pipe == 0 && !wr_valid) begin
                sram_ce_n    <= 1;
                sram_oe_n    <= 1;
                sram_we_n    <= 1;
                sram_data_oe <= 0;
            end
        end
    end

endmodule
