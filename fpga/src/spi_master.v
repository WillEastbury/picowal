`timescale 1ns / 1ps
//============================================================================
// spi_master.v — Generic SPI Master (Mode 0) for iCE40 HX8K
//
// SPI Mode 0: CPOL=0, CPHA=0 — clock idles low, data sampled on rising edge,
// shifted out on falling edge. 8-bit transfers, MSB first.
//
// CS is caller-managed: drive cs_assert high to pull spi_cs_n low.
//============================================================================

module spi_master #(
    parameter CLK_DIV = 4   // spi_clk = clk / (2 * CLK_DIV)
                            // e.g. 100 MHz / 8 = 12.5 MHz SPI clock
)(
    // System
    input  wire       clk,
    input  wire       rst_n,

    // Control / data interface
    input  wire       start,        // pulse high for one clk to begin transfer
    input  wire [7:0] tx_data,      // byte to transmit (latched on start)
    output reg  [7:0] rx_data,      // received byte (valid when done pulses)
    output reg        done,         // pulses high for one clk when transfer ends

    // Caller-managed chip select
    input  wire       cs_assert,    // 1 = assert CS (drive spi_cs_n low)

    // SPI bus
    output reg        spi_clk,
    output reg        spi_mosi,
    input  wire       spi_miso,
    output wire       spi_cs_n
);

    // ── CS pass-through ─────────────────────────────────────────────────
    assign spi_cs_n = ~cs_assert;

    // ── State encoding ──────────────────────────────────────────────────
    localparam S_IDLE    = 1'b0;
    localparam S_RUNNING = 1'b1;

    // Minimum 1-bit counter width (guards CLK_DIV == 1 where $clog2 = 0)
    localparam CNT_W   = (CLK_DIV > 1) ? $clog2(CLK_DIV) : 1;
    localparam [CNT_W-1:0] CLK_MAX = CLK_DIV - 1;

    reg        state;
    reg [7:0]  shift_reg;       // TX shift register (MSB out first)
    reg [7:0]  rx_shift;        // RX shift register (MSB in first)
    reg [2:0]  bit_cnt;         // counts 0‥7
    reg [CNT_W-1:0] clk_cnt;   // clock-divider counter
    reg        spi_clk_r;      // internal copy of SPI clock level

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= S_IDLE;
            shift_reg <= 8'd0;
            rx_shift  <= 8'd0;
            rx_data   <= 8'd0;
            done      <= 1'b0;
            bit_cnt   <= 3'd0;
            clk_cnt   <= {CNT_W{1'b0}};
            spi_clk_r <= 1'b0;
            spi_clk   <= 1'b0;
            spi_mosi  <= 1'b0;
        end else begin
            done <= 1'b0;               // default: one-cycle pulse

            case (state)
                // ── IDLE ────────────────────────────────────────────────
                S_IDLE: begin
                    spi_clk_r <= 1'b0;
                    spi_clk   <= 1'b0;
                    if (start) begin
                        shift_reg <= tx_data;       // latch transmit byte
                        spi_mosi  <= tx_data[7];    // drive MSB immediately
                        rx_shift  <= 8'd0;
                        bit_cnt   <= 3'd0;
                        clk_cnt   <= {CNT_W{1'b0}};
                        spi_clk_r <= 1'b0;
                        state     <= S_RUNNING;
                    end
                end

                // ── RUNNING ─────────────────────────────────────────────
                S_RUNNING: begin
                    if (clk_cnt == CLK_MAX) begin
                        clk_cnt   <= {CNT_W{1'b0}};
                        spi_clk_r <= ~spi_clk_r;
                        spi_clk   <= ~spi_clk_r;

                        if (~spi_clk_r) begin
                            // ── Rising edge of SPI clock: sample MISO ───
                            rx_shift <= {rx_shift[6:0], spi_miso};
                        end else begin
                            // ── Falling edge of SPI clock: advance bit ──
                            if (bit_cnt == 3'd7) begin
                                // Last bit completed — rx_shift already
                                // holds all 8 sampled bits from rising edges
                                rx_data <= rx_shift;
                                done    <= 1'b1;
                                state   <= S_IDLE;
                            end else begin
                                bit_cnt   <= bit_cnt + 3'd1;
                                shift_reg <= {shift_reg[6:0], 1'b0};
                                spi_mosi  <= shift_reg[6]; // next MSB
                            end
                        end
                    end else begin
                        clk_cnt <= clk_cnt + 1'b1;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
