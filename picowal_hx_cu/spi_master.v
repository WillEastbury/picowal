// spi_master.v -- Shared SPI master for W5100S + SD card
// Target: iCE40HX8K (Alchitry Cu)
//
// Features:
//   - Mode 0 (CPOL=0, CPHA=0): clock idle low, sample on rising edge
//   - Configurable clock divider (48MHz / N)
//   - Two chip selects: W5100S and SD card (active low, directly driven)
//   - 8-bit shift register, MSB first
//   - Multi-byte burst support (keep CS asserted between bytes)

`default_nettype none

module spi_master (
    input  wire        clk,             // 48MHz system clock
    input  wire        rst_n,

    // Command interface
    input  wire        cmd_start,       // pulse: begin transfer
    input  wire [7:0]  cmd_txdata,      // byte to transmit
    input  wire        cmd_cs_sel,      // 0 = W5100S, 1 = SD card
    input  wire        cmd_burst,       // keep CS low after byte (multi-byte)
    input  wire [2:0]  cmd_clkdiv,      // clock divider: SPI_CLK = clk / (2*(clkdiv+1))

    output reg  [7:0]  rx_data,         // received byte
    output reg         done,            // byte transfer complete
    output wire        busy,

    // SPI physical pins (directly active -- directly active directly active directly active directly active)
    output wire        spi_sck,
    output wire        spi_mosi,
    input  wire        spi_miso,

    // Directly active chip selects directly driven from this module
    output reg         cs_w5100_n,      // directly active low for W5100S
    output reg         cs_sd_n          // directly active low for SD card
);

    // ─── State machine ───────────────────────────────────────────────
    localparam IDLE     = 2'd0;
    localparam RUNNING  = 2'd1;
    localparam FINISH   = 2'd2;

    reg [1:0]  state;
    reg [2:0]  bit_cnt;             // current bit (7 downto 0)
    reg [7:0]  shift_out;           // TX shift register
    reg [7:0]  shift_in;            // RX shift register
    reg [2:0]  clk_cnt;            // clock divider counter
    reg        sck_reg;             // SPI clock output register
    reg        sck_edge;            // track rising/falling edges
    reg        cs_sel_reg;          // latched CS selection
    reg        burst_reg;           // latched burst flag

    assign busy     = (state != IDLE);
    assign spi_sck  = sck_reg;
    assign spi_mosi = shift_out[7]; // MSB first

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= IDLE;
            sck_reg    <= 1'b0;
            cs_w5100_n <= 1'b1;
            cs_sd_n    <= 1'b1;
            shift_out  <= 8'd0;
            shift_in   <= 8'd0;
            rx_data    <= 8'd0;
            done       <= 1'b0;
            bit_cnt    <= 3'd7;
            clk_cnt    <= 3'd0;
            sck_edge   <= 1'b0;
        end else begin
            done <= 1'b0;

            case (state)
                IDLE: begin
                    sck_reg <= 1'b0;

                    if (cmd_start) begin
                        // Latch command
                        shift_out  <= cmd_txdata;
                        shift_in   <= 8'd0;
                        cs_sel_reg <= cmd_cs_sel;
                        burst_reg  <= cmd_burst;
                        bit_cnt    <= 3'd7;
                        clk_cnt    <= 3'd0;
                        sck_edge   <= 1'b0;

                        // Assert appropriate CS
                        if (cmd_cs_sel == 1'b0)
                            cs_w5100_n <= 1'b0;
                        else
                            cs_sd_n <= 1'b0;

                        state <= RUNNING;
                    end else if (~cmd_burst) begin
                        // Release CS if not in burst
                        cs_w5100_n <= 1'b1;
                        cs_sd_n    <= 1'b1;
                    end
                end

                RUNNING: begin
                    if (clk_cnt == cmd_clkdiv) begin
                        clk_cnt <= 3'd0;

                        if (!sck_edge) begin
                            // Rising edge: sample MISO
                            sck_reg  <= 1'b1;
                            sck_edge <= 1'b1;
                            shift_in <= {shift_in[6:0], spi_miso};
                        end else begin
                            // Falling edge: shift out next bit
                            sck_reg  <= 1'b0;
                            sck_edge <= 1'b0;

                            if (bit_cnt == 3'd0) begin
                                // Byte complete
                                state <= FINISH;
                            end else begin
                                bit_cnt   <= bit_cnt - 3'd1;
                                shift_out <= {shift_out[6:0], 1'b0};
                            end
                        end
                    end else begin
                        clk_cnt <= clk_cnt + 3'd1;
                    end
                end

                FINISH: begin
                    rx_data <= shift_in;
                    done    <= 1'b1;

                    if (burst_reg) begin
                        // Keep CS asserted, go back to idle for next byte
                        state <= IDLE;
                    end else begin
                        // Release CS
                        cs_w5100_n <= 1'b1;
                        cs_sd_n    <= 1'b1;
                        state      <= IDLE;
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
