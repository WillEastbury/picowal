`timescale 1ns / 1ps
//============================================================================
// w5500_spi.v — W5500 SPI Driver for iCE40 HX8K
//
// Low-level register and buffer access over SPI using the W5500 3-byte
// header frame format:
//   Byte 0: Address[15:8]
//   Byte 1: Address[7:0]
//   Byte 2: {BSB[4:0], R/W, OM[1:0]}  (OM=00 variable-length mode)
//
// Supports four operations:
//   reg_read  — 3-byte header + 1 data byte read
//   reg_write — 3-byte header + 1 data byte write
//   buf_read  — 3-byte header + buf_len data bytes read
//   buf_write — 3-byte header + buf_len data bytes write
//
// Uses a single spi_master instance. CS is managed by the FSM.
//============================================================================

module w5500_spi #(
    parameter CLK_DIV = 4   // passed through to spi_master
)(
    // System
    input  wire        clk,
    input  wire        rst_n,

    // ── Single-register interface ────────────────────────────────────────
    input  wire        reg_start,       // pulse to begin reg read/write
    input  wire        reg_write,       // 0 = read, 1 = write
    input  wire [15:0] reg_addr,        // W5500 register address
    input  wire  [4:0] reg_bsb,         // Block Select Bits
    input  wire  [7:0] reg_wdata,       // write data (ignored on read)
    output reg   [7:0] reg_rdata,       // read data (valid on reg_done)
    output reg         reg_done,        // pulses high for one cycle

    // ── Buffer (multi-byte) interface ────────────────────────────────────
    input  wire        buf_start,       // pulse to begin buffer transfer
    input  wire        buf_write,       // 0 = read, 1 = write
    input  wire [15:0] buf_addr,        // start address in buffer
    input  wire  [4:0] buf_bsb,         // Block Select Bits
    input  wire [10:0] buf_len,         // number of data bytes (1–2048)
    input  wire  [7:0] buf_wdata,       // next write byte (sampled when buf_wvalid)
    input  wire        buf_wvalid,      // caller asserts when buf_wdata is ready
    output reg   [7:0] buf_rdata,       // read byte output
    output reg         buf_rvalid,      // pulses high when buf_rdata is valid
    output reg         buf_done,        // pulses high when entire transfer done

    // ── SPI pins to W5500 ────────────────────────────────────────────────
    output wire        w5500_cs_n,
    output wire        w5500_sck,
    output wire        w5500_mosi,
    input  wire        w5500_miso
);

    // =====================================================================
    // FSM state encoding
    // =====================================================================
    localparam [2:0] ST_IDLE      = 3'd0,
                     ST_SEND_HDR0 = 3'd1,
                     ST_SEND_HDR1 = 3'd2,
                     ST_SEND_HDR2 = 3'd3,
                     ST_DATA_LOOP = 3'd4,
                     ST_DONE      = 3'd5;

    reg [2:0]  state;

    // ── Latched command parameters ───────────────────────────────────────
    reg [15:0] cmd_addr;
    reg  [4:0] cmd_bsb;
    reg        cmd_rw;          // 0=read, 1=write
    reg        cmd_is_buf;      // 0=register (1 byte), 1=buffer (multi)
    reg [10:0] cmd_len;         // data-phase byte count
    reg [10:0] byte_cnt;        // bytes remaining in data phase
    reg  [7:0] cmd_wdata;       // single-register write data

    // ── SPI master wires ─────────────────────────────────────────────────
    reg        spi_start;
    reg  [7:0] spi_tx;
    reg        spi_cs;          // 1 = assert CS (active)
    wire [7:0] spi_rx;
    wire       spi_done;

    // Header byte 2: {BSB[4:0], R/W, OM[1:0]=00}
    wire [7:0] hdr_byte2 = {cmd_bsb, cmd_rw, 2'b00};

    // =====================================================================
    // SPI master instance
    // =====================================================================
    spi_master #(
        .CLK_DIV(CLK_DIV)
    ) u_spi (
        .clk       (clk),
        .rst_n     (rst_n),
        .start     (spi_start),
        .tx_data   (spi_tx),
        .rx_data   (spi_rx),
        .done      (spi_done),
        .cs_assert (spi_cs),
        .spi_clk   (w5500_sck),
        .spi_mosi  (w5500_mosi),
        .spi_miso  (w5500_miso),
        .spi_cs_n  (w5500_cs_n)
    );

    // =====================================================================
    // Main FSM
    // =====================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= ST_IDLE;
            spi_start  <= 1'b0;
            spi_tx     <= 8'd0;
            spi_cs     <= 1'b0;
            reg_rdata  <= 8'd0;
            reg_done   <= 1'b0;
            buf_rdata  <= 8'd0;
            buf_rvalid <= 1'b0;
            buf_done   <= 1'b0;
            cmd_addr   <= 16'd0;
            cmd_bsb    <= 5'd0;
            cmd_rw     <= 1'b0;
            cmd_is_buf <= 1'b0;
            cmd_len    <= 11'd0;
            cmd_wdata  <= 8'd0;
            byte_cnt   <= 11'd0;
        end else begin
            // Default pulse signals low
            spi_start  <= 1'b0;
            reg_done   <= 1'b0;
            buf_rvalid <= 1'b0;
            buf_done   <= 1'b0;

            case (state)
                // ─────────────────────────────────────────────────────────
                // IDLE — wait for a command
                // ─────────────────────────────────────────────────────────
                ST_IDLE: begin
                    if (reg_start) begin
                        // Latch register command
                        cmd_addr   <= reg_addr;
                        cmd_bsb    <= reg_bsb;
                        cmd_rw     <= reg_write;
                        cmd_is_buf <= 1'b0;
                        cmd_len    <= 11'd1;
                        cmd_wdata  <= reg_wdata;
                        // Assert CS, send address high byte
                        spi_cs     <= 1'b1;
                        spi_tx     <= reg_addr[15:8];
                        spi_start  <= 1'b1;
                        state      <= ST_SEND_HDR0;
                    end else if (buf_start) begin
                        // Latch buffer command
                        cmd_addr   <= buf_addr;
                        cmd_bsb    <= buf_bsb;
                        cmd_rw     <= buf_write;
                        cmd_is_buf <= 1'b1;
                        cmd_len    <= buf_len;
                        cmd_wdata  <= 8'd0;
                        // Assert CS, send address high byte
                        spi_cs     <= 1'b1;
                        spi_tx     <= buf_addr[15:8];
                        spi_start  <= 1'b1;
                        state      <= ST_SEND_HDR0;
                    end
                end

                // ─────────────────────────────────────────────────────────
                // SEND_HDR0 — wait for addr[15:8] to finish, send addr[7:0]
                // ─────────────────────────────────────────────────────────
                ST_SEND_HDR0: begin
                    if (spi_done) begin
                        spi_tx    <= cmd_addr[7:0];
                        spi_start <= 1'b1;
                        state     <= ST_SEND_HDR1;
                    end
                end

                // ─────────────────────────────────────────────────────────
                // SEND_HDR1 — wait for addr[7:0] to finish, send control
                // ─────────────────────────────────────────────────────────
                ST_SEND_HDR1: begin
                    if (spi_done) begin
                        spi_tx    <= hdr_byte2;
                        spi_start <= 1'b1;
                        state     <= ST_SEND_HDR2;
                    end
                end

                // ─────────────────────────────────────────────────────────
                // SEND_HDR2 — wait for control byte, enter data phase
                // ─────────────────────────────────────────────────────────
                ST_SEND_HDR2: begin
                    if (spi_done) begin
                        byte_cnt <= cmd_len;
                        if (cmd_rw) begin
                            // Write: send first data byte
                            if (cmd_is_buf) begin
                                // Buffer write: data comes from buf_wdata
                                // Wait for buf_wvalid before sending
                                if (buf_wvalid) begin
                                    spi_tx    <= buf_wdata;
                                    spi_start <= 1'b1;
                                    byte_cnt  <= cmd_len - 11'd1;
                                    state     <= ST_DATA_LOOP;
                                end
                                // else stay here waiting for wvalid
                            end else begin
                                // Register write: single byte from cmd_wdata
                                spi_tx    <= cmd_wdata;
                                spi_start <= 1'b1;
                                byte_cnt  <= 11'd0;
                                state     <= ST_DATA_LOOP;
                            end
                        end else begin
                            // Read: clock out dummy byte to receive data
                            spi_tx    <= 8'h00;
                            spi_start <= 1'b1;
                            byte_cnt  <= cmd_len - 11'd1;
                            state     <= ST_DATA_LOOP;
                        end
                    end
                end

                // ─────────────────────────────────────────────────────────
                // DATA_LOOP — transfer data bytes (read or write)
                // ─────────────────────────────────────────────────────────
                ST_DATA_LOOP: begin
                    if (spi_done) begin
                        if (!cmd_rw) begin
                            // Read: capture received byte
                            if (cmd_is_buf) begin
                                buf_rdata  <= spi_rx;
                                buf_rvalid <= 1'b1;
                            end else begin
                                reg_rdata <= spi_rx;
                            end
                        end

                        if (byte_cnt == 11'd0) begin
                            // All bytes transferred — release CS
                            state <= ST_DONE;
                        end else begin
                            // More bytes to transfer
                            if (cmd_rw) begin
                                // Write next byte
                                if (cmd_is_buf) begin
                                    if (buf_wvalid) begin
                                        spi_tx    <= buf_wdata;
                                        spi_start <= 1'b1;
                                        byte_cnt  <= byte_cnt - 11'd1;
                                    end
                                    // else stay waiting for buf_wvalid
                                end else begin
                                    // Should not happen for reg (len=1)
                                    state <= ST_DONE;
                                end
                            end else begin
                                // Read next byte (send dummy)
                                spi_tx    <= 8'h00;
                                spi_start <= 1'b1;
                                byte_cnt  <= byte_cnt - 11'd1;
                            end
                        end
                    end else if (cmd_rw && cmd_is_buf && (byte_cnt != 11'd0)) begin
                        // Buffer write: waiting for spi_done — also check
                        // if we need to handle wvalid pipelining (no-op here,
                        // handled above on spi_done)
                    end
                end

                // ─────────────────────────────────────────────────────────
                // DONE — deassert CS, signal completion
                // ─────────────────────────────────────────────────────────
                ST_DONE: begin
                    spi_cs <= 1'b0;
                    if (cmd_is_buf)
                        buf_done <= 1'b1;
                    else
                        reg_done <= 1'b1;
                    state <= ST_IDLE;
                end

                default: begin
                    spi_cs <= 1'b0;
                    state  <= ST_IDLE;
                end
            endcase
        end
    end

endmodule
