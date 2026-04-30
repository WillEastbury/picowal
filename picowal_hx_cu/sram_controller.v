// sram_controller.v -- Async SRAM controller for IS61WV25616BLL
// Target: iCE40HX8K (Alchitry Cu)
//
// 512KB (256K × 16-bit) async SRAM, 10ns access time
// At 48MHz (20.8ns period): 2 cycles per read, 2 cycles per write
// Supports burst mode for PIPE/FOREACH (back-to-back accesses)

`default_nettype none

module sram_controller (
    input  wire        clk,
    input  wire        rst_n,

    // Command interface (from execution unit)
    input  wire        cmd_read,        // pulse: start read
    input  wire        cmd_write,       // pulse: start write
    input  wire [17:0] cmd_addr,        // byte address → word address internally
    input  wire [15:0] cmd_wdata,       // write data
    input  wire        cmd_burst,       // stay active for sequential access

    output reg  [15:0] rdata,           // read data (valid when done)
    output reg         done,            // operation complete
    output wire        busy,

    // SRAM physical interface
    output reg  [17:0] sram_addr,
    inout  wire [15:0] sram_data,
    output reg         sram_ce_n,       // chip enable (active low)
    output reg         sram_oe_n,       // output enable (active low)
    output reg         sram_we_n,       // write enable (active low)
    output reg         sram_lb_n,       // lower byte enable
    output reg         sram_ub_n        // upper byte enable
);

    // ─── State machine ───────────────────────────────────────────────
    localparam IDLE      = 3'd0;
    localparam READ_S1   = 3'd1;    // address setup
    localparam READ_S2   = 3'd2;    // data valid, latch
    localparam WRITE_S1  = 3'd3;    // address + data setup
    localparam WRITE_S2  = 3'd4;    // WE# pulse width
    localparam DONE_S    = 3'd5;    // signal completion

    reg [2:0] state;
    reg [15:0] wdata_reg;
    reg drive_data;                  // 1 = we drive the data bus (write)

    assign busy = (state != IDLE);

    // Tristate data bus control
    assign sram_data = drive_data ? wdata_reg : 16'hZZZZ;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= IDLE;
            sram_ce_n  <= 1'b1;
            sram_oe_n  <= 1'b1;
            sram_we_n  <= 1'b1;
            sram_lb_n  <= 1'b0;
            sram_ub_n  <= 1'b0;
            drive_data <= 1'b0;
            done       <= 1'b0;
            rdata      <= 16'd0;
        end else begin
            done <= 1'b0;

            case (state)
                IDLE: begin
                    sram_ce_n  <= 1'b1;
                    sram_oe_n  <= 1'b1;
                    sram_we_n  <= 1'b1;
                    drive_data <= 1'b0;

                    if (cmd_read) begin
                        sram_addr <= cmd_addr;
                        sram_ce_n <= 1'b0;
                        sram_oe_n <= 1'b0;
                        sram_lb_n <= 1'b0;
                        sram_ub_n <= 1'b0;
                        state     <= READ_S1;
                    end else if (cmd_write) begin
                        sram_addr  <= cmd_addr;
                        wdata_reg  <= cmd_wdata;
                        sram_ce_n  <= 1'b0;
                        sram_lb_n  <= 1'b0;
                        sram_ub_n  <= 1'b0;
                        drive_data <= 1'b1;
                        state      <= WRITE_S1;
                    end
                end

                // ─── READ: cycle 1 = address setup time ──────────────
                READ_S1: begin
                    state <= READ_S2;
                end

                // ─── READ: cycle 2 = data valid, latch it ────────────
                READ_S2: begin
                    rdata <= sram_data;
                    done  <= 1'b1;

                    if (cmd_burst & cmd_read) begin
                        // Back-to-back burst: next address already valid
                        sram_addr <= cmd_addr;
                        state     <= READ_S1;
                    end else begin
                        sram_ce_n <= 1'b1;
                        sram_oe_n <= 1'b1;
                        state     <= IDLE;
                    end
                end

                // ─── WRITE: cycle 1 = address + data setup ───────────
                WRITE_S1: begin
                    sram_we_n <= 1'b0;   // assert write enable
                    state     <= WRITE_S2;
                end

                // ─── WRITE: cycle 2 = WE# pulse complete ─────────────
                WRITE_S2: begin
                    sram_we_n  <= 1'b1;  // deassert write enable
                    drive_data <= 1'b0;
                    done       <= 1'b1;

                    if (cmd_burst & cmd_write) begin
                        // Back-to-back burst write
                        sram_addr  <= cmd_addr;
                        wdata_reg  <= cmd_wdata;
                        drive_data <= 1'b1;
                        state      <= WRITE_S1;
                    end else begin
                        sram_ce_n <= 1'b1;
                        state     <= IDLE;
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
