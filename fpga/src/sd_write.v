`timescale 1ns / 1ps
//============================================================================
// sd_write.v — SD card SPI single/multi-block write FSM for iCE40 HX8K
//
// CMD24 (single) or CMD25 (multi-block) with CRC16 generation.
// Data sourced via data_req/data_in handshake. For multi-block, pulse 'stop'
// to send the stop-tran token (0xFD).
//============================================================================

module sd_write (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,
    input  wire [31:0] block_addr,
    input  wire        multi,       // 1=CMD25, 0=CMD24
    input  wire        stop,        // send stop token
    input  wire [7:0]  data_in,
    input  wire        data_wr,     // pulse when data_in valid
    // SPI master control
    output reg         spi_start,
    output reg  [7:0]  spi_tx_data,
    output reg         spi_cs_assert,
    input  wire [7:0]  spi_rx_data,
    input  wire        spi_done,
    // CRC16 interface
    output reg         crc_clear,
    output reg         crc_enable,
    output reg         crc_data_in,
    input  wire [15:0] crc_out,
    // Control
    output reg         data_req,    // request next byte from host
    output reg         block_done,
    output reg         error,
    output reg         busy
);

    // ── State encoding ──────────────────────────────────────────────────
    localparam [4:0]
        S_IDLE          = 5'd0,
        S_SEND_CMD      = 5'd1,
        S_WAIT_R1       = 5'd2,
        S_SEND_TOKEN    = 5'd3,
        S_REQ_DATA      = 5'd4,
        S_WAIT_DATA     = 5'd5,
        S_FEED_CRC      = 5'd6,
        S_SEND_BYTE     = 5'd7,
        S_SEND_CRC_HI   = 5'd8,
        S_SEND_CRC_LO   = 5'd9,
        S_WAIT_RESP     = 5'd10,
        S_WAIT_BUSY     = 5'd11,
        S_BLOCK_DONE    = 5'd12,
        S_STOP_TOKEN    = 5'd13,
        S_STOP_BUSY     = 5'd14,
        S_FINISH        = 5'd15,
        S_ERROR         = 5'd16;

    reg [4:0]  state;

    // Command buffer
    reg [47:0] cmd_buf;
    reg [2:0]  cmd_idx;

    // Counters
    reg [3:0]  poll_cnt;
    reg [9:0]  data_cnt;       // 0..511
    reg [2:0]  crc_bit_idx;
    reg [7:0]  crc_feed_byte;

    // Captured CRC for sending
    reg [15:0] tx_crc;

    // Latch multi mode
    reg        multi_r;
    reg        stop_req;

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
            data_req      <= 1'b0;
            block_done    <= 1'b0;
            error         <= 1'b0;
            busy          <= 1'b0;
            cmd_buf       <= 48'd0;
            cmd_idx       <= 3'd0;
            poll_cnt      <= 4'd0;
            data_cnt      <= 10'd0;
            crc_bit_idx   <= 3'd0;
            crc_feed_byte <= 8'd0;
            tx_crc        <= 16'd0;
            multi_r       <= 1'b0;
            stop_req      <= 1'b0;
        end else begin
            // Defaults
            spi_start  <= 1'b0;
            data_req   <= 1'b0;
            block_done <= 1'b0;
            crc_clear  <= 1'b0;
            crc_enable <= 1'b0;

            // Latch stop
            if (stop) stop_req <= 1'b1;

            case (state)

            // ────────────────────────────────────────────────────────────
            S_IDLE: begin
                error <= 1'b0;
                if (start) begin
                    busy      <= 1'b1;
                    multi_r   <= multi;
                    stop_req  <= 1'b0;
                    // CMD24=0x58, CMD25=0x59
                    cmd_buf   <= {multi ? 8'h59 : 8'h58,
                                  block_addr[31:24], block_addr[23:16],
                                  block_addr[15:8],  block_addr[7:0],
                                  8'hFF};
                    cmd_idx       <= 3'd0;
                    spi_cs_assert <= 1'b1;
                    spi_tx_data   <= multi ? 8'h59 : 8'h58;
                    spi_start     <= 1'b1;
                    state         <= S_SEND_CMD;
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
                        poll_cnt    <= 4'd0;
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_WAIT_R1;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for R1 response
            // ────────────────────────────────────────────────────────────
            S_WAIT_R1: begin
                if (spi_done) begin
                    if (spi_rx_data != 8'hFF) begin
                        if (spi_rx_data == 8'h00) begin
                            state <= S_SEND_TOKEN;
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
            // Send data start token
            // ────────────────────────────────────────────────────────────
            S_SEND_TOKEN: begin
                // 0xFE for single/first block, 0xFC for multi-block
                spi_tx_data <= multi_r ? 8'hFC : 8'hFE;
                spi_start   <= 1'b1;
                data_cnt    <= 10'd0;
                crc_clear   <= 1'b1;
                state       <= S_REQ_DATA;
            end

            // ────────────────────────────────────────────────────────────
            // Request data byte from host
            // ────────────────────────────────────────────────────────────
            S_REQ_DATA: begin
                if (spi_done) begin
                    data_req <= 1'b1;
                    state    <= S_WAIT_DATA;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for host to provide data byte
            // ────────────────────────────────────────────────────────────
            S_WAIT_DATA: begin
                if (data_wr) begin
                    crc_feed_byte <= data_in;
                    crc_bit_idx   <= 3'd7;
                    state         <= S_FEED_CRC;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Feed 8 bits of data byte into CRC (one bit per clock)
            // ────────────────────────────────────────────────────────────
            S_FEED_CRC: begin
                crc_enable  <= 1'b1;
                crc_data_in <= crc_feed_byte[crc_bit_idx];
                if (crc_bit_idx == 3'd0) begin
                    // All bits fed — send the byte over SPI
                    spi_tx_data <= crc_feed_byte;
                    spi_start   <= 1'b1;
                    state       <= S_SEND_BYTE;
                end else begin
                    crc_bit_idx <= crc_bit_idx - 3'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for SPI byte to finish, then request next or send CRC
            // ────────────────────────────────────────────────────────────
            S_SEND_BYTE: begin
                if (spi_done) begin
                    if (data_cnt == 10'd511) begin
                        // All 512 bytes sent — capture CRC and send it
                        tx_crc      <= crc_out;
                        spi_tx_data <= crc_out[15:8];
                        spi_start   <= 1'b1;
                        state       <= S_SEND_CRC_HI;
                    end else begin
                        data_cnt <= data_cnt + 10'd1;
                        data_req <= 1'b1;
                        state    <= S_WAIT_DATA;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Send CRC high byte
            // ────────────────────────────────────────────────────────────
            S_SEND_CRC_HI: begin
                if (spi_done) begin
                    spi_tx_data <= tx_crc[7:0];
                    spi_start   <= 1'b1;
                    state       <= S_SEND_CRC_LO;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Send CRC low byte, then wait for data response token
            // ────────────────────────────────────────────────────────────
            S_SEND_CRC_LO: begin
                if (spi_done) begin
                    spi_tx_data <= 8'hFF;
                    spi_start   <= 1'b1;
                    state       <= S_WAIT_RESP;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Check data response token (xxx0_sss1 where sss=010 = accepted)
            // ────────────────────────────────────────────────────────────
            S_WAIT_RESP: begin
                if (spi_done) begin
                    if ((spi_rx_data & 8'h1F) == 8'h05) begin
                        // Data accepted — wait for busy to clear
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                        state       <= S_WAIT_BUSY;
                    end else if (spi_rx_data == 8'hFF) begin
                        // No response yet, keep polling
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end else begin
                        // CRC error or write error
                        state <= S_ERROR;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Wait for card to finish programming (MISO goes high)
            // ────────────────────────────────────────────────────────────
            S_WAIT_BUSY: begin
                if (spi_done) begin
                    if (spi_rx_data == 8'h00) begin
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end else begin
                        state <= S_BLOCK_DONE;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // Block complete — loop or finish
            // ────────────────────────────────────────────────────────────
            S_BLOCK_DONE: begin
                block_done <= 1'b1;
                if (multi_r && !stop_req) begin
                    // Next block — send token immediately
                    state <= S_SEND_TOKEN;
                end else if (multi_r && stop_req) begin
                    // Send stop-tran token 0xFD
                    spi_tx_data <= 8'hFD;
                    spi_start   <= 1'b1;
                    state       <= S_STOP_TOKEN;
                end else begin
                    // Single block done
                    spi_cs_assert <= 1'b0;
                    busy          <= 1'b0;
                    state         <= S_FINISH;
                end
            end

            // ────────────────────────────────────────────────────────────
            // Stop-tran token sent — wait for busy
            // ────────────────────────────────────────────────────────────
            S_STOP_TOKEN: begin
                if (spi_done) begin
                    // Send one stuff byte then poll busy
                    spi_tx_data <= 8'hFF;
                    spi_start   <= 1'b1;
                    state       <= S_STOP_BUSY;
                end
            end

            S_STOP_BUSY: begin
                if (spi_done) begin
                    if (spi_rx_data == 8'h00) begin
                        spi_tx_data <= 8'hFF;
                        spi_start   <= 1'b1;
                    end else begin
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
