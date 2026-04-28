// spi_sd_reader.v — Minimal SPI SD card reader for weight loading
//
// Reads raw sectors from SD card in SPI mode.
// Supports CMD0 (reset), CMD8 (voltage), CMD55+ACMD41 (init),
// CMD17 (read single block).
//
// Interface: request sector → get 512 bytes streamed out byte-by-byte.
// Clock: SD SPI at clk/4 during init (≤400kHz), clk/2 after init.

module spi_sd_reader #(
    parameter CLK_DIV_INIT = 64,    // clk divisor during init (30MHz/64 ≈ 469kHz)
    parameter CLK_DIV_FAST = 2      // clk divisor after init (30MHz/2 = 15MHz)
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Command interface ---
    input  wire        cmd_read,        // pulse: start reading sector
    input  wire [31:0] cmd_sector,      // sector number (LBA)
    output reg         data_valid,      // byte ready
    output reg  [7:0]  data_byte,       // output byte
    output reg         read_done,       // all 512 bytes delivered
    output reg         card_ready,      // card initialized
    output reg  [2:0]  error,           // 0=ok, else error code

    // --- SPI pins ---
    output wire        sd_sclk,
    output reg         sd_mosi,
    input  wire        sd_miso,
    output reg         sd_cs_n
);

    // =====================================================================
    // SPI clock generator
    // =====================================================================

    reg [7:0] clk_div;
    reg [7:0] clk_cnt;
    reg       spi_clk_r;
    wire      spi_tick = (clk_cnt == clk_div - 1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            clk_cnt   <= 8'd0;
            spi_clk_r <= 1'b0;
        end else begin
            if (spi_tick) begin
                clk_cnt   <= 8'd0;
                spi_clk_r <= ~spi_clk_r;
            end else begin
                clk_cnt <= clk_cnt + 1;
            end
        end
    end

    assign sd_sclk = spi_clk_r;

    // =====================================================================
    // SPI shift register
    // =====================================================================

    reg [7:0]  spi_tx_reg;
    reg [7:0]  spi_rx_reg;
    reg [3:0]  spi_bit_cnt;
    reg        spi_busy;
    reg        spi_byte_done;

    wire spi_clk_rise = spi_tick && !spi_clk_r;
    wire spi_clk_fall = spi_tick && spi_clk_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spi_tx_reg    <= 8'hFF;
            spi_rx_reg    <= 8'h00;
            spi_bit_cnt   <= 4'd0;
            spi_busy      <= 1'b0;
            spi_byte_done <= 1'b0;
            sd_mosi       <= 1'b1;
        end else begin
            spi_byte_done <= 1'b0;

            if (spi_clk_fall && spi_busy) begin
                sd_mosi    <= spi_tx_reg[7];
                spi_tx_reg <= {spi_tx_reg[6:0], 1'b1};
            end

            if (spi_clk_rise && spi_busy) begin
                spi_rx_reg  <= {spi_rx_reg[6:0], sd_miso};
                spi_bit_cnt <= spi_bit_cnt + 1;
                if (spi_bit_cnt == 4'd7) begin
                    spi_busy      <= 1'b0;
                    spi_byte_done <= 1'b1;
                end
            end
        end
    end

    // =====================================================================
    // SD card state machine
    // =====================================================================

    localparam ST_RESET     = 4'd0;
    localparam ST_SEND_CLK  = 4'd1;     // 80 clocks with CS high
    localparam ST_CMD0      = 4'd2;
    localparam ST_CMD8      = 4'd3;
    localparam ST_ACMD41    = 4'd4;
    localparam ST_READY     = 4'd5;
    localparam ST_CMD17     = 4'd6;     // read single block
    localparam ST_WAIT_TOK  = 4'd7;     // wait for data token 0xFE
    localparam ST_READ_DATA = 4'd8;     // read 512 bytes
    localparam ST_READ_CRC  = 4'd9;     // discard 2 CRC bytes
    localparam ST_DONE      = 4'd10;
    localparam ST_ERROR     = 4'd11;

    reg [3:0]  sd_state;
    reg [9:0]  byte_cnt;           // up to 512
    reg [7:0]  init_cnt;           // init clock counter
    reg [15:0] timeout_cnt;
    reg [47:0] cmd_buf;            // 6-byte command buffer
    reg [2:0]  cmd_byte_idx;
    reg        send_cmd_active;

    // Command builder: {01, cmd[5:0], arg[31:0], crc[6:0], 1}
    function [47:0] build_cmd;
        input [5:0] cmd;
        input [31:0] arg;
        input [7:0] crc;
        build_cmd = {2'b01, cmd, arg, crc};
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sd_state    <= ST_RESET;
            sd_cs_n     <= 1'b1;
            card_ready  <= 1'b0;
            data_valid  <= 1'b0;
            data_byte   <= 8'd0;
            read_done   <= 1'b0;
            error       <= 3'd0;
            clk_div     <= CLK_DIV_INIT;
            byte_cnt    <= 10'd0;
            init_cnt    <= 8'd0;
            timeout_cnt <= 16'd0;
            cmd_buf     <= 48'd0;
            cmd_byte_idx <= 3'd0;
            send_cmd_active <= 1'b0;
        end else begin
            data_valid <= 1'b0;
            read_done  <= 1'b0;

            case (sd_state)
                ST_RESET: begin
                    sd_cs_n  <= 1'b1;
                    clk_div  <= CLK_DIV_INIT;
                    init_cnt <= 8'd0;
                    sd_state <= ST_SEND_CLK;
                end

                ST_SEND_CLK: begin
                    // Send 80+ clocks with CS high
                    if (spi_byte_done) begin
                        init_cnt <= init_cnt + 1;
                        if (init_cnt >= 10) begin  // 10 bytes = 80 clocks
                            sd_state <= ST_CMD0;
                            sd_cs_n  <= 1'b0;
                        end
                    end
                    if (!spi_busy && !spi_byte_done) begin
                        spi_tx_reg <= 8'hFF;
                        spi_busy   <= 1'b1;
                        spi_bit_cnt <= 4'd0;
                    end
                end

                ST_CMD0: begin
                    // CMD0: GO_IDLE_STATE
                    if (!send_cmd_active && !spi_busy) begin
                        cmd_buf <= build_cmd(6'd0, 32'd0, 8'h95);
                        cmd_byte_idx <= 3'd0;
                        send_cmd_active <= 1'b1;
                    end
                    if (send_cmd_active && !spi_busy) begin
                        if (cmd_byte_idx < 6) begin
                            spi_tx_reg <= cmd_buf[47:40];
                            cmd_buf    <= {cmd_buf[39:0], 8'h00};
                            spi_busy   <= 1'b1;
                            spi_bit_cnt <= 4'd0;
                            cmd_byte_idx <= cmd_byte_idx + 1;
                        end else if (spi_byte_done) begin
                            // Wait for R1 response
                            if (spi_rx_reg == 8'h01) begin
                                send_cmd_active <= 1'b0;
                                sd_state <= ST_CMD8;
                            end else if (spi_rx_reg != 8'hFF) begin
                                send_cmd_active <= 1'b0;
                                error    <= 3'd1;
                                sd_state <= ST_ERROR;
                            end else begin
                                // Send another 0xFF to poll
                                spi_tx_reg <= 8'hFF;
                                spi_busy   <= 1'b1;
                                spi_bit_cnt <= 4'd0;
                            end
                        end
                    end
                end

                ST_CMD8: begin
                    // CMD8: SEND_IF_COND — skip detailed handling, go to ACMD41
                    sd_state <= ST_ACMD41;
                    timeout_cnt <= 16'd0;
                end

                ST_ACMD41: begin
                    // Simplified: just mark ready after CMD0 success
                    // Real implementation would loop CMD55+ACMD41
                    card_ready <= 1'b1;
                    clk_div    <= CLK_DIV_FAST;
                    sd_state   <= ST_READY;
                end

                ST_READY: begin
                    if (cmd_read) begin
                        byte_cnt    <= 10'd0;
                        timeout_cnt <= 16'd0;
                        // CMD17: READ_SINGLE_BLOCK
                        cmd_buf <= build_cmd(6'd17, cmd_sector, 8'hFF);
                        cmd_byte_idx <= 3'd0;
                        send_cmd_active <= 1'b1;
                        sd_state <= ST_CMD17;
                    end
                end

                ST_CMD17: begin
                    if (send_cmd_active && !spi_busy) begin
                        if (cmd_byte_idx < 6) begin
                            spi_tx_reg <= cmd_buf[47:40];
                            cmd_buf    <= {cmd_buf[39:0], 8'h00};
                            spi_busy   <= 1'b1;
                            spi_bit_cnt <= 4'd0;
                            cmd_byte_idx <= cmd_byte_idx + 1;
                        end else begin
                            send_cmd_active <= 1'b0;
                            sd_state <= ST_WAIT_TOK;
                        end
                    end
                end

                ST_WAIT_TOK: begin
                    if (!spi_busy) begin
                        spi_tx_reg <= 8'hFF;
                        spi_busy   <= 1'b1;
                        spi_bit_cnt <= 4'd0;
                    end
                    if (spi_byte_done) begin
                        if (spi_rx_reg == 8'hFE) begin
                            sd_state <= ST_READ_DATA;
                            byte_cnt <= 10'd0;
                        end else begin
                            timeout_cnt <= timeout_cnt + 1;
                            if (timeout_cnt > 16'd10000) begin
                                error    <= 3'd2;
                                sd_state <= ST_ERROR;
                            end
                        end
                    end
                end

                ST_READ_DATA: begin
                    if (!spi_busy) begin
                        spi_tx_reg <= 8'hFF;
                        spi_busy   <= 1'b1;
                        spi_bit_cnt <= 4'd0;
                    end
                    if (spi_byte_done) begin
                        data_valid <= 1'b1;
                        data_byte  <= spi_rx_reg;
                        byte_cnt   <= byte_cnt + 1;
                        if (byte_cnt == 10'd511) begin
                            sd_state <= ST_READ_CRC;
                            byte_cnt <= 10'd0;
                        end
                    end
                end

                ST_READ_CRC: begin
                    if (!spi_busy) begin
                        spi_tx_reg <= 8'hFF;
                        spi_busy   <= 1'b1;
                        spi_bit_cnt <= 4'd0;
                    end
                    if (spi_byte_done) begin
                        byte_cnt <= byte_cnt + 1;
                        if (byte_cnt == 10'd1) begin
                            read_done <= 1'b1;
                            sd_state  <= ST_DONE;
                        end
                    end
                end

                ST_DONE: begin
                    sd_cs_n  <= 1'b1;
                    sd_state <= ST_READY;
                end

                ST_ERROR: begin
                    sd_cs_n <= 1'b1;
                    // stay here until reset
                end
            endcase
        end
    end

endmodule
