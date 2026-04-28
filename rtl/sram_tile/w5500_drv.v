// w5500_drv.v — W5500 Wiznet Ethernet SPI driver
//
// Provides register read/write + socket buffer access over SPI.
// W5500 SPI frame: [addr_hi][addr_lo][control][data...]
//   control = {BSB[4:0], RW, OM[1:0]}
//   BSB: 00000=common, 00001=S0_REG, 00010=S0_TX, 00011=S0_RX, ...
//   RW: 0=read, 1=write
//   OM: 00=VDM (variable length), 01=FDM1, 10=FDM2, 11=FDM4
//
// This driver uses VDM (variable data length mode) for all transfers.

module w5500_drv #(
    parameter CLK_DIV = 2        // SPI clock = clk / (2 * CLK_DIV)
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Command interface ---
    input  wire        cmd_valid,
    output reg         cmd_ready,
    input  wire        cmd_rw,       // 0=read, 1=write
    input  wire [4:0]  cmd_bsb,      // block select
    input  wire [15:0] cmd_addr,     // register address
    input  wire [7:0]  cmd_wdata,    // write data (single byte)
    output reg  [7:0]  cmd_rdata,    // read data (single byte)
    output reg         cmd_done,     // transfer complete pulse

    // --- Bulk transfer interface ---
    input  wire        bulk_valid,
    output reg         bulk_ready,
    input  wire        bulk_rw,      // 0=read, 1=write
    input  wire [4:0]  bulk_bsb,
    input  wire [15:0] bulk_addr,
    input  wire [10:0] bulk_len,     // up to 2048 bytes
    // Bulk write: caller provides bytes via bulk_wdata/bulk_wdata_valid
    input  wire [7:0]  bulk_wdata,
    input  wire        bulk_wdata_valid,
    // Bulk read: driver outputs bytes via bulk_rdata/bulk_rdata_valid
    output reg  [7:0]  bulk_rdata,
    output reg         bulk_rdata_valid,
    output reg         bulk_done,

    // --- SPI pins ---
    output reg         spi_sclk,
    output reg         spi_mosi,
    input  wire        spi_miso,
    output reg         spi_cs_n
);

    // =====================================================================
    // SPI clock divider
    // =====================================================================

    reg [7:0] clk_cnt;
    wire      spi_tick = (clk_cnt == CLK_DIV - 1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            clk_cnt <= 8'd0;
        else if (spi_tick)
            clk_cnt <= 8'd0;
        else
            clk_cnt <= clk_cnt + 1;
    end

    // =====================================================================
    // SPI shift engine
    // =====================================================================

    reg [7:0]  shift_out;
    reg [7:0]  shift_in;
    reg [3:0]  bit_cnt;
    reg        shifting;
    reg        shift_done;

    wire spi_clk_fall = spi_tick && spi_sclk;
    wire spi_clk_rise = spi_tick && !spi_sclk;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spi_sclk   <= 1'b0;
            spi_mosi   <= 1'b1;
            shift_in   <= 8'd0;
            shift_out  <= 8'hFF;
            bit_cnt    <= 4'd0;
            shifting   <= 1'b0;
            shift_done <= 1'b0;
        end else begin
            shift_done <= 1'b0;

            if (shifting) begin
                if (spi_clk_fall) begin
                    // Drive MOSI on falling edge
                    spi_mosi  <= shift_out[7];
                    shift_out <= {shift_out[6:0], 1'b1};
                    spi_sclk  <= 1'b0;
                end
                if (spi_clk_rise) begin
                    // Sample MISO on rising edge
                    shift_in <= {shift_in[6:0], spi_miso};
                    spi_sclk <= 1'b1;
                    bit_cnt  <= bit_cnt + 1;
                    if (bit_cnt == 4'd7) begin
                        shifting   <= 1'b0;
                        shift_done <= 1'b1;
                    end
                end
            end else begin
                spi_sclk <= 1'b0;
            end
        end
    end

    // Start shifting a byte
    task start_shift;
        input [7:0] data;
        begin
            shift_out <= data;
            bit_cnt   <= 4'd0;
            shifting  <= 1'b1;
        end
    endtask

    // =====================================================================
    // Main state machine
    // =====================================================================

    localparam S_IDLE      = 4'd0;
    localparam S_HDR0      = 4'd1;  // send addr_hi
    localparam S_HDR1      = 4'd2;  // send addr_lo
    localparam S_HDR2      = 4'd3;  // send control byte
    localparam S_DATA      = 4'd4;  // send/recv data byte
    localparam S_FINISH    = 4'd5;
    localparam S_BULK_HDR0 = 4'd6;
    localparam S_BULK_HDR1 = 4'd7;
    localparam S_BULK_HDR2 = 4'd8;
    localparam S_BULK_DATA = 4'd9;
    localparam S_BULK_WAIT = 4'd10; // wait for write data from caller
    localparam S_BULK_FIN  = 4'd11;

    reg [3:0]  state;
    reg        rw_r;
    reg [4:0]  bsb_r;
    reg [15:0] addr_r;
    reg [10:0] bulk_len_r;
    reg [10:0] bulk_idx;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= S_IDLE;
            cmd_ready       <= 1'b1;
            cmd_rdata       <= 8'd0;
            cmd_done        <= 1'b0;
            bulk_ready      <= 1'b1;
            bulk_rdata      <= 8'd0;
            bulk_rdata_valid <= 1'b0;
            bulk_done       <= 1'b0;
            spi_cs_n        <= 1'b1;
            rw_r            <= 1'b0;
            bsb_r           <= 5'd0;
            addr_r          <= 16'd0;
            bulk_len_r      <= 11'd0;
            bulk_idx        <= 11'd0;
        end else begin
            cmd_done         <= 1'b0;
            bulk_rdata_valid <= 1'b0;
            bulk_done        <= 1'b0;

            case (state)
                // ---------------------------------------------------------
                // Single register access
                // ---------------------------------------------------------
                S_IDLE: begin
                    spi_cs_n <= 1'b1;

                    if (cmd_valid && cmd_ready) begin
                        cmd_ready <= 1'b0;
                        rw_r      <= cmd_rw;
                        bsb_r     <= cmd_bsb;
                        addr_r    <= cmd_addr;
                        spi_cs_n  <= 1'b0;
                        start_shift(cmd_addr[15:8]);
                        state     <= S_HDR0;
                    end else if (bulk_valid && bulk_ready) begin
                        bulk_ready <= 1'b0;
                        rw_r       <= bulk_rw;
                        bsb_r      <= bulk_bsb;
                        addr_r     <= bulk_addr;
                        bulk_len_r <= bulk_len;
                        bulk_idx   <= 11'd0;
                        spi_cs_n   <= 1'b0;
                        start_shift(bulk_addr[15:8]);
                        state      <= S_BULK_HDR0;
                    end
                end

                S_HDR0: begin
                    if (shift_done && !shifting) begin
                        start_shift(addr_r[7:0]);
                        state <= S_HDR1;
                    end
                end

                S_HDR1: begin
                    if (shift_done && !shifting) begin
                        // Control: {BSB[4:0], RW, OM[1:0]=00}
                        start_shift({bsb_r, rw_r, 2'b00});
                        state <= S_HDR2;
                    end
                end

                S_HDR2: begin
                    if (shift_done && !shifting) begin
                        if (rw_r)
                            start_shift(cmd_wdata);
                        else
                            start_shift(8'hFF);  // dummy for read
                        state <= S_DATA;
                    end
                end

                S_DATA: begin
                    if (shift_done && !shifting) begin
                        if (!rw_r)
                            cmd_rdata <= shift_in;
                        state <= S_FINISH;
                    end
                end

                S_FINISH: begin
                    spi_cs_n  <= 1'b1;
                    cmd_done  <= 1'b1;
                    cmd_ready <= 1'b1;
                    state     <= S_IDLE;
                end

                // ---------------------------------------------------------
                // Bulk transfer
                // ---------------------------------------------------------
                S_BULK_HDR0: begin
                    if (shift_done && !shifting) begin
                        start_shift(addr_r[7:0]);
                        state <= S_BULK_HDR1;
                    end
                end

                S_BULK_HDR1: begin
                    if (shift_done && !shifting) begin
                        start_shift({bsb_r, rw_r, 2'b00});
                        state <= S_BULK_HDR2;
                    end
                end

                S_BULK_HDR2: begin
                    if (shift_done && !shifting) begin
                        if (rw_r) begin
                            state <= S_BULK_WAIT;  // wait for first write byte
                        end else begin
                            start_shift(8'hFF);
                            state <= S_BULK_DATA;
                        end
                    end
                end

                S_BULK_WAIT: begin
                    // Wait for caller to provide write data
                    if (bulk_wdata_valid) begin
                        start_shift(bulk_wdata);
                        state <= S_BULK_DATA;
                    end
                end

                S_BULK_DATA: begin
                    if (shift_done && !shifting) begin
                        if (!rw_r) begin
                            bulk_rdata       <= shift_in;
                            bulk_rdata_valid <= 1'b1;
                        end

                        bulk_idx <= bulk_idx + 1;

                        if (bulk_idx + 1 >= bulk_len_r) begin
                            state <= S_BULK_FIN;
                        end else if (rw_r) begin
                            state <= S_BULK_WAIT;
                        end else begin
                            start_shift(8'hFF);
                        end
                    end
                end

                S_BULK_FIN: begin
                    spi_cs_n   <= 1'b1;
                    bulk_done  <= 1'b1;
                    bulk_ready <= 1'b1;
                    state      <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
