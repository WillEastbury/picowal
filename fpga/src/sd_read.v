`timescale 1ns / 1ps
//============================================================================
// sd_read.v — SD card SPI single/multi-block read FSM for iCE40 HX8K
//
// CMD17 (single) or CMD18 (multi-block) with CRC16 verification.
// For multi-block, loops back after each block; pulse 'stop' to send CMD12.
//============================================================================

module sd_read (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,
    input  wire [31:0] block_addr,
    input  wire        multi,       // 1=CMD18, 0=CMD17
    input  wire        stop,        // pulse to send CMD12
    // SPI master control
    output reg         spi_start,
    output reg  [7:0]  spi_tx_data,
    output reg         spi_cs_assert,
    input  wire [7:0]  spi_rx_data,
    input  wire        spi_done,
    // CRC16 interface (serial, directly connected to crc16 module)
    output reg         crc_clear,
    output reg         crc_enable,
    output reg         crc_data_in,
    input  wire [15:0] crc_out,
    // Data output
    output reg  [7:0]  data_out,
    output reg         data_valid,
    output reg         block_done,
    output reg         error,
    output reg         busy
);

    // ── State encoding ──────────────────────────────────────────────────
    localparam [3:0]
        S_IDLE       = 4'd0,
        S_SEND_CMD   = 4'd1,
        S_WAIT_R1    = 4'd2,
        S_WAIT_TOKEN = 4'd3,
        S_READ_DATA  = 4'd4,
        S_FEED_CRC   = 4'd5,
        S_READ_CRC   = 4'd6,
        S_CHECK_CRC  = 4'd7,
        S_BLOCK_DONE = 4'd8,
        S_STOP_CMD   = 4'd9,
        S_STOP_R1    = 4'd10,
        S_STOP_BUSY  = 4'd11,
        S_FINISH     = 4'd12,
        S_ERROR      = 4'd13;

    reg [3:0]  state;

    // Command buffer
    reg [47:0] cmd_buf;
    reg [2:0]  cmd_idx;        // 0..5 for 6 command bytes

    // Poll / data counters
    reg [3:0]  poll_cnt;       // R1 poll attempts
    reg [9:0]  data_cnt;       // 0..511 for 512 data bytes
    reg [15:0] token_cnt;      // timeout for data token
    reg [2:0]  crc_bit_idx;    // bit feeder index 7..0
    reg [1:0]  crc_byte_cnt;   // 0..1 for 2 CRC bytes
    reg [15:0] rx_crc;         // received CRC16
    reg [7:0]  crc_feed_byte;  // byte being fed to CRC serially

    // Latch multi mode
    reg        multi_r;
    reg        stop_req;       // latched stop request

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state         <= S_IDLE;
            spi_start     <= 1'b0;
            spi_tx_data   <= 8'hFF;
            spi_cs_assert <= 1'b0;
            crc_clear     <= 1'b0;
            crc_enable    <= 1'b0;
            crc_data_in   <= 1'b0;
            data_out      <= 8'd0;
            data_valid    <= 1'b0;
            block_done    <= 1'b0;
            error         <= 1'b0;
            busy          <= 1'b0;
            cmd_buf       <= 48'd0;
            cmd_idx       <= 3'd0;
            poll_cnt      <= 4'd0;
            data_cnt      <= 10'd0;
            token_cnt     <= 16'd0;
            crc_bit_idx   <= 3'd0;
            crc_byte_cnt  <= 2'd0;
            rx_crc        <= 16'd0;
            crc_feed_byte <= 8'd0;
            multi_r       <= 1'b0;
            stop_req      <= 1'b0;
        end else begin
            // Defaults
            spi_start  <= 1'b0;
            data_valid <= 1'b0;
            block_done <= 1'b0;
            crc_clear  <= 1'b0;
            crc_enable <= 1'b0;

            // Latch stop request
            if (stop) stop_req <= 1'b1;

            case (state)

            // ────────────────────────────────────────────────────────────
            S_IDLE: begin
                error <= 1'b0;
                if (start) begin
                    busy      <= 1'b1;
                    multi_r   <= multi;
                    stop_req  <= 1'b0;
                    // CMD17=0x51, CMD18=0x52
                    cmd_buf   <= {multi ? 8'h52 : 8'h51,
                                  block_addr[31:24], block_addr[23:16],
                                  block_addr[15:8],  block_addr[7:0],
                                  8'hFF};  // CRC don't-care in SPI mode
                    cmd_idx   <= 3'd0;
                    spi_cs_assert <= 1'b1;
                    spi_tx_data   <= multi ? 8'h52 : 8'h51;
                    spi_start     <= 1'b1;
                    state     <= S_SEND_CMD;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Send 6-byte command
            // ────────────────────────────────────────────────────────────
            S_SEND_CMD: begin
                if (spi_done) begin
                    if (cmd_idx < 3'd5) begin
                        cmd_idx <= cmd_idx + 3'd1;
                        case (cmd_idx + 3'd1)
                            3'd1: begin spi_tx_data <= cmd_buf[39:32]; spi_start <= 1'b1; end
                            3'd2: begin spi_tx_data <= cmd_buf[31:24]; spi_start <= 1'b1; end
                            3'd3: begin spi_tx_data <= cmd_buf[23:16]; spi_start <= 1'b1; end
                            3'd4: begin spi_tx_data <= cmd_buf[15:8];  spi_start <= 1'b1; end
                            3'd5: begin spi_tx_data <= cmd_buf[7:0];   spi_start <= 1'b1; end
                            default: ;
                        endcase
                    end else begin
                        // Poll for R1
                        poll_cnt    <= 4'd0;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_WAIT_R1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for R1 response (non-0xFF)
            // ────────────────────────────────────────────────────────────
            S_WAIT_R1: begin
                if (spi_done) begin
                    if (spi_rx_data != 8'hFF) begin
                        if (spi_rx_data == 8'h00) begin
                            // Good R1 — wait for data token
                            token_cnt   <= 16'd0;
                            crc_clear   <= 1'b1;
                            spi_tx_data <= 8'hFF;
                            spi_start   <= 1'b1;
                            state       <= S_WAIT_TOKEN;
                        end else begin
                            state <= S_ERROR;
                        end
                    end else if (poll_cnt == 4'd9) begin
                        state <= S_ERROR;
                    end else begin
                        poll_cnt    <= poll_cnt + 4'd1;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for data start token 0xFE
            // ────────────────────────────────────────────────────────────
            S_WAIT_TOKEN: begin
                if (spi_done) begin
                    if (spi_rx_data == 8'hFE) begin
                        // Token received — start reading 512 data bytes
                        data_cnt    <= 10'd0;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_READ_DATA;
                    end else if (spi_rx_data != 8'hFF) begin
                        // Error token
                        state <= S_ERROR;
                    end else if (token_cnt >= 16'hFFFF) begin
                        state <= S_ERROR;
                    end else begin
                        token_cnt   <= token_cnt + 16'd1;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Read 512 data bytes, feeding CRC bit-by-bit after each byte
            // ────────────────────────────────────────────────────────────
            S_READ_DATA: begin
                if (spi_done) begin
                    data_out      <= spi_rx_data;
                    data_valid    <= 1'b1;
                    crc_feed_byte <= spi_rx_data;
                    crc_bit_idx   <= 3'd7;
                    state         <= S_FEED_CRC;
                end
            end

            // Feed 8 bits to CRC (combinational loop, one bit per clock)
            S_FEED_CRC: begin
                crc_enable  <= 1'b1;
                crc_data_in <= crc_feed_byte[crc_bit_idx];
                if (crc_bit_idx == 3'd0) begin
                    // All 8 bits fed
                    if (data_cnt == 10'd511) begin
                        // 512 bytes done — read 2 CRC bytes
                        crc_byte_cnt <= 2'd0;
                        rx_crc       <= 16'd0;
                        spi_tx_data  <= 8'hFF;
                        spi_start    <= 1'b1;
                        state        <= S_READ_CRC;
                    end else begin
                        data_cnt    <= data_cnt + 10'd1;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_READ_DATA;
                    end
                end else begin
                    crc_bit_idx <= crc_bit_idx - 3'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Read 2 CRC bytes
            // ────────────────────────────────────────────────────────────
            S_READ_CRC: begin
                if (spi_done) begin
                    rx_crc <= {rx_crc[7:0], spi_rx_data};
                    if (crc_byte_cnt == 2'd1) begin
                        state <= S_CHECK_CRC;
                    end else begin
                        crc_byte_cnt <= crc_byte_cnt + 2'd1;
                        spi_tx_data  <= 8'hFF;
                        spi_start    <= 1'b1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Verify CRC
            // ────────────────────────────────────────────────────────────
            S_CHECK_CRC: begin
                if (rx_crc != crc_out) begin
                    state <= S_ERROR;
                end else begin
                    state <= S_BLOCK_DONE;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Block complete
            // ────────────────────────────────────────────────────────────
            S_BLOCK_DONE: begin
                block_done <= 1'b1;
                if (multi_r && !stop_req) begin
                    // Wait for next data token
                    token_cnt   <= 16'd0;
                    crc_clear   <= 1'b1;
                    spi_tx_data <= 8'hFF;
                    spi_start   <= 1'b1;
                    state       <= S_WAIT_TOKEN;
                end else if (multi_r && stop_req) begin
                    // Send CMD12 to stop transmission
                    cmd_buf <= {8'h4C, 8'h00, 8'h00, 8'h00, 8'h00, 8'hFF};
                    cmd_idx <= 3'd0;
                    spi_tx_data <= 8'h4C;
                    spi_start   <= 1'b1;
                    state       <= S_STOP_CMD;
                end else begin
                    // Single block — done
                    spi_cs_assert <= 1'b0;
                    busy          <= 1'b0;
                    state         <= S_FINISH;
                end
            end

            // ────────────────────────────────────────────────────────────
            // CMD12 — STOP_TRANSMISSION
            // ────────────────────────────────────────────────────────────
            S_STOP_CMD: begin
                if (spi_done) begin
                    if (cmd_idx < 3'd5) begin
                        cmd_idx <= cmd_idx + 3'd1;
                        case (cmd_idx + 3'd1)
                            3'd1: begin spi_tx_data <= cmd_buf[39:32]; spi_start <= 1'b1; end
                            3'd2: begin spi_tx_data <= cmd_buf[31:24]; spi_start <= 1'b1; end
                            3'd3: begin spi_tx_data <= cmd_buf[23:16]; spi_start <= 1'b1; end
                            3'd4: begin spi_tx_data <= cmd_buf[15:8];  spi_start <= 1'b1; end
                            3'd5: begin spi_tx_data <= cmd_buf[7:0];   spi_start <= 1'b1; end
                            default: ;
                        endcase
                    end else begin
                        // Skip stuff byte then poll for R1
                        poll_cnt    <= 4'd0;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_STOP_R1;
                    end
                end
            end

            S_STOP_R1: begin
                if (spi_done) begin
                    if (spi_rx_data != 8'hFF) begin
                        // Got R1 — now wait for card to finish busy
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_STOP_BUSY;
                    end else if (poll_cnt == 4'd9) begin
                        state <= S_ERROR;
                    end else begin
                        poll_cnt    <= poll_cnt + 4'd1;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end
                end
            end

            S_STOP_BUSY: begin
                if (spi_done) begin
                    if (spi_rx_data == 8'h00) begin
                        // Card still busy
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end else begin
                        // Card no longer busy
                        spi_cs_assert <= 1'b0;
                        busy          <= 1'b0;
                        state         <= S_FINISH;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            S_FINISH: begin
                state <= S_IDLE;
            end

            S_ERROR: begin
                error         <= 1'b1;
                busy          <= 1'b0;
                spi_cs_assert <= 1'b0;
                state         <= S_IDLE;
            end

            default: state <= S_ERROR;

            endcase
        end
    end

endmodule
