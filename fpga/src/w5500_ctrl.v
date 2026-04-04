`timescale 1ns / 1ps
//============================================================================
// w5500_ctrl.v — W5500 Initialization & Socket 0 TCP Server Controller
//
// High-level controller that:
//   1. Resets and configures the W5500 (gateway, subnet, MAC, IP)
//   2. Opens socket 0 as a TCP server on a configurable port
//   3. Manages connection lifecycle (listen → accept → data → close)
//   4. Provides a streaming interface for an HTTP parser
//
// Connection FSM:
//   INIT → LISTEN → WAIT_CONNECT → ESTABLISHED → CLOSE_WAIT → DISCONNECT → LISTEN
//
// Synthesizable Verilog-2001 for Lattice iCE40 HX8K.
//============================================================================

module w5500_ctrl #(
    // ── Network configuration (defaults) ─────────────────────────────────
    parameter [31:0] IP_ADDR    = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [31:0] GATEWAY    = {8'd192, 8'd168, 8'd1, 8'd1},
    parameter [31:0] SUBNET     = {8'd255, 8'd255, 8'd255, 8'd0},
    parameter [47:0] MAC_ADDR   = {8'hDE, 8'hAD, 8'hBE, 8'hEF, 8'h00, 8'h01},
    parameter [15:0] LISTEN_PORT = 16'd80,
    // ── Timing ───────────────────────────────────────────────────────────
    parameter CLK_DIV    = 4,             // SPI clock divider
    parameter RESET_WAIT = 20'd100_000    // clocks to wait after SW reset
)(
    // System
    input  wire        clk,
    input  wire        rst_n,

    // ── Status ───────────────────────────────────────────────────────────
    output reg         init_done,         // high after W5500 fully configured
    output reg         client_connected,  // high while SOCK_ESTABLISHED

    // ── RX interface (to HTTP parser) ────────────────────────────────────
    output reg   [7:0] rx_data,           // received byte
    output reg         rx_valid,          // pulses when rx_data is valid
    output reg  [15:0] rx_len,            // total bytes available from client
    input  wire        rx_consume,        // pulse: parser finished with RX data

    // ── TX interface (from HTTP parser) ──────────────────────────────────
    input  wire  [7:0] tx_data,           // byte to transmit
    input  wire        tx_valid,          // asserted when tx_data is ready
    input  wire [15:0] tx_len,            // total bytes to send
    input  wire        tx_start,          // pulse to begin TX transfer
    output reg         tx_done,           // pulses when TX transfer complete

    // ── SPI pins to W5500 ────────────────────────────────────────────────
    output wire        w5500_cs_n,
    output wire        w5500_sck,
    output wire        w5500_mosi,
    input  wire        w5500_miso
);

    // =====================================================================
    // W5500 register addresses — Common (BSB = 0x00)
    // =====================================================================
    localparam [15:0] MR_ADDR   = 16'h0000;  // Mode Register
    localparam [15:0] GAR_ADDR  = 16'h0001;  // Gateway (4 bytes)
    localparam [15:0] SUBR_ADDR = 16'h0005;  // Subnet Mask (4 bytes)
    localparam [15:0] SHAR_ADDR = 16'h0009;  // MAC Address (6 bytes)
    localparam [15:0] SIPR_ADDR = 16'h000F;  // Source IP (4 bytes)

    // =====================================================================
    // Socket 0 register addresses (BSB = 0x01)
    // =====================================================================
    localparam [15:0] SN_MR_ADDR     = 16'h0000;  // Socket Mode
    localparam [15:0] SN_CR_ADDR     = 16'h0001;  // Socket Command
    localparam [15:0] SN_SR_ADDR     = 16'h0003;  // Socket Status
    localparam [15:0] SN_PORT_ADDR   = 16'h0004;  // Source Port (2 bytes)
    localparam [15:0] SN_TX_FSR_ADDR = 16'h0020;  // TX Free Size (2 bytes)
    localparam [15:0] SN_TX_WR_ADDR  = 16'h0024;  // TX Write Pointer (2 bytes)
    localparam [15:0] SN_RX_RSR_ADDR = 16'h0026;  // RX Received Size (2 bytes)
    localparam [15:0] SN_RX_RD_ADDR  = 16'h0028;  // RX Read Pointer (2 bytes)

    // BSB constants
    localparam [4:0] BSB_COMMON = 5'b00000;   // Common registers
    localparam [4:0] BSB_S0_REG = 5'b00001;   // Socket 0 registers
    localparam [4:0] BSB_S0_TX  = 5'b00010;   // Socket 0 TX buffer
    localparam [4:0] BSB_S0_RX  = 5'b00011;   // Socket 0 RX buffer

    // Socket commands
    localparam [7:0] CMD_OPEN    = 8'h01;
    localparam [7:0] CMD_LISTEN  = 8'h02;
    localparam [7:0] CMD_DISCON  = 8'h08;
    localparam [7:0] CMD_CLOSE   = 8'h10;
    localparam [7:0] CMD_SEND    = 8'h20;
    localparam [7:0] CMD_RECV    = 8'h40;

    // Socket status values
    localparam [7:0] SOCK_CLOSED      = 8'h00;
    localparam [7:0] SOCK_INIT        = 8'h13;
    localparam [7:0] SOCK_LISTEN      = 8'h14;
    localparam [7:0] SOCK_ESTABLISHED = 8'h17;
    localparam [7:0] SOCK_CLOSE_WAIT  = 8'h1C;

    // =====================================================================
    // w5500_spi instance wires
    // =====================================================================
    reg        reg_start_r;
    reg        reg_write_r;
    reg [15:0] reg_addr_r;
    reg  [4:0] reg_bsb_r;
    reg  [7:0] reg_wdata_r;
    wire [7:0] reg_rdata_w;
    wire       reg_done_w;

    reg        buf_start_r;
    reg        buf_write_r;
    reg [15:0] buf_addr_r;
    reg  [4:0] buf_bsb_r;
    reg [10:0] buf_len_r;
    reg  [7:0] buf_wdata_r;
    reg        buf_wvalid_r;
    wire [7:0] buf_rdata_w;
    wire       buf_rvalid_w;
    wire       buf_done_w;

    w5500_spi #(
        .CLK_DIV(CLK_DIV)
    ) u_spi (
        .clk        (clk),
        .rst_n      (rst_n),
        .reg_start  (reg_start_r),
        .reg_write  (reg_write_r),
        .reg_addr   (reg_addr_r),
        .reg_bsb    (reg_bsb_r),
        .reg_wdata  (reg_wdata_r),
        .reg_rdata  (reg_rdata_w),
        .reg_done   (reg_done_w),
        .buf_start  (buf_start_r),
        .buf_write  (buf_write_r),
        .buf_addr   (buf_addr_r),
        .buf_bsb    (buf_bsb_r),
        .buf_len    (buf_len_r),
        .buf_wdata  (buf_wdata_r),
        .buf_wvalid (buf_wvalid_r),
        .buf_rdata  (buf_rdata_w),
        .buf_rvalid (buf_rvalid_w),
        .buf_done   (buf_done_w),
        .w5500_cs_n (w5500_cs_n),
        .w5500_sck  (w5500_sck),
        .w5500_mosi (w5500_mosi),
        .w5500_miso (w5500_miso)
    );

    // =====================================================================
    // Init sequence ROM — {addr[15:0], bsb[4:0], data[7:0]} per write
    // We store each byte write as an entry. Total = 1+4+4+6+4+1+2 = 22 writes,
    // plus socket commands handled procedurally.
    // =====================================================================
    localparam INIT_LEN = 22;

    reg [28:0] init_rom [0:INIT_LEN-1]; // {addr[15:0], bsb[4:0], data[7:0]}
    reg  [4:0] init_idx;

    // Build init ROM combinatorially — packs addr, bsb, data
    // Using a function-style generate to keep it readable
    // Format: {16'addr, 5'bsb, 8'data}
    initial begin
        // Software reset: MR = 0x80
        init_rom[ 0] = {MR_ADDR,   BSB_COMMON, 8'h80};
        // Gateway address (4 bytes)
        init_rom[ 1] = {GAR_ADDR + 16'd0, BSB_COMMON, GATEWAY[31:24]};
        init_rom[ 2] = {GAR_ADDR + 16'd1, BSB_COMMON, GATEWAY[23:16]};
        init_rom[ 3] = {GAR_ADDR + 16'd2, BSB_COMMON, GATEWAY[15:8]};
        init_rom[ 4] = {GAR_ADDR + 16'd3, BSB_COMMON, GATEWAY[7:0]};
        // Subnet mask (4 bytes)
        init_rom[ 5] = {SUBR_ADDR + 16'd0, BSB_COMMON, SUBNET[31:24]};
        init_rom[ 6] = {SUBR_ADDR + 16'd1, BSB_COMMON, SUBNET[23:16]};
        init_rom[ 7] = {SUBR_ADDR + 16'd2, BSB_COMMON, SUBNET[15:8]};
        init_rom[ 8] = {SUBR_ADDR + 16'd3, BSB_COMMON, SUBNET[7:0]};
        // MAC address (6 bytes)
        init_rom[ 9] = {SHAR_ADDR + 16'd0, BSB_COMMON, MAC_ADDR[47:40]};
        init_rom[10] = {SHAR_ADDR + 16'd1, BSB_COMMON, MAC_ADDR[39:32]};
        init_rom[11] = {SHAR_ADDR + 16'd2, BSB_COMMON, MAC_ADDR[31:24]};
        init_rom[12] = {SHAR_ADDR + 16'd3, BSB_COMMON, MAC_ADDR[23:16]};
        init_rom[13] = {SHAR_ADDR + 16'd4, BSB_COMMON, MAC_ADDR[15:8]};
        init_rom[14] = {SHAR_ADDR + 16'd5, BSB_COMMON, MAC_ADDR[7:0]};
        // Source IP address (4 bytes)
        init_rom[15] = {SIPR_ADDR + 16'd0, BSB_COMMON, IP_ADDR[31:24]};
        init_rom[16] = {SIPR_ADDR + 16'd1, BSB_COMMON, IP_ADDR[23:16]};
        init_rom[17] = {SIPR_ADDR + 16'd2, BSB_COMMON, IP_ADDR[15:8]};
        init_rom[18] = {SIPR_ADDR + 16'd3, BSB_COMMON, IP_ADDR[7:0]};
        // Socket 0: Mode = TCP
        init_rom[19] = {SN_MR_ADDR, BSB_S0_REG, 8'h01};
        // Socket 0: Source port (big-endian)
        init_rom[20] = {SN_PORT_ADDR + 16'd0, BSB_S0_REG, LISTEN_PORT[15:8]};
        init_rom[21] = {SN_PORT_ADDR + 16'd1, BSB_S0_REG, LISTEN_PORT[7:0]};
    end

    // =====================================================================
    // Master FSM state encoding
    // =====================================================================
    localparam [4:0] S_RESET_WAIT     =  5'd0,
                     S_INIT_WRITE     =  5'd1,
                     S_INIT_WAIT      =  5'd2,
                     S_RESET_VERIFY   =  5'd3,
                     S_RESET_CHECK    =  5'd4,
                     S_SOCK_OPEN      =  5'd5,
                     S_SOCK_OPEN_WAIT =  5'd6,
                     S_POLL_INIT      =  5'd7,
                     S_POLL_INIT_CHK  =  5'd8,
                     S_SOCK_LISTEN    =  5'd9,
                     S_SOCK_LISN_WAIT =  5'd10,
                     S_POLL_LISTEN    =  5'd11,
                     S_POLL_LISN_CHK  =  5'd12,
                     S_WAIT_CONNECT   =  5'd13,
                     S_CHK_CONNECT    =  5'd14,
                     S_ESTABLISHED    =  5'd15,
                     S_READ_RSR_H     =  5'd16,
                     S_READ_RSR_L     =  5'd17,
                     S_READ_RSR_CHK   =  5'd18,
                     S_READ_RD_H      =  5'd19,
                     S_READ_RD_L      =  5'd20,
                     S_RX_BUF_READ    =  5'd21,
                     S_RX_UPDATE_RD   =  5'd22,
                     S_RX_CMD_RECV    =  5'd23,
                     S_TX_READ_WR_H   =  5'd24,
                     S_TX_READ_WR_L   =  5'd25,
                     S_TX_BUF_WRITE   =  5'd26,
                     S_TX_UPDATE_WR   =  5'd27,
                     S_TX_CMD_SEND    =  5'd28,
                     S_DISCONNECT     =  5'd29,
                     S_DISC_WAIT      =  5'd30,
                     S_CLOSE_CMD      =  5'd31;

    reg  [4:0] state;
    reg [19:0] wait_cnt;         // general-purpose wait counter

    // ── Data-path registers ──────────────────────────────────────────────
    reg [15:0] rx_rsr;           // RX received-size register
    reg [15:0] rx_rd_ptr;        // RX read pointer
    reg [15:0] tx_wr_ptr;        // TX write pointer
    reg [15:0] tx_remaining;     // TX bytes remaining
    reg  [7:0] sock_status;      // last-read socket status
    reg        tx_active;        // TX transfer in progress

    // Sub-step counter for multi-register reads/writes
    reg  [1:0] sub_step;

    // =====================================================================
    // Helper task: issue a single register write
    // =====================================================================
    task issue_reg_write;
        input [15:0] addr;
        input  [4:0] bsb;
        input  [7:0] data;
    begin
        reg_addr_r  <= addr;
        reg_bsb_r   <= bsb;
        reg_wdata_r  <= data;
        reg_write_r  <= 1'b1;
        reg_start_r  <= 1'b1;
    end
    endtask

    // =====================================================================
    // Helper task: issue a single register read
    // =====================================================================
    task issue_reg_read;
        input [15:0] addr;
        input  [4:0] bsb;
    begin
        reg_addr_r  <= addr;
        reg_bsb_r   <= bsb;
        reg_write_r  <= 1'b0;
        reg_start_r  <= 1'b1;
    end
    endtask

    // =====================================================================
    // Master FSM
    // =====================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= S_RESET_WAIT;
            wait_cnt        <= 20'd0;
            init_idx        <= 5'd0;
            init_done       <= 1'b0;
            client_connected <= 1'b0;
            rx_data         <= 8'd0;
            rx_valid        <= 1'b0;
            rx_len          <= 16'd0;
            tx_done         <= 1'b0;
            tx_active       <= 1'b0;
            reg_start_r     <= 1'b0;
            reg_write_r     <= 1'b0;
            reg_addr_r      <= 16'd0;
            reg_bsb_r       <= 5'd0;
            reg_wdata_r     <= 8'd0;
            buf_start_r     <= 1'b0;
            buf_write_r     <= 1'b0;
            buf_addr_r      <= 16'd0;
            buf_bsb_r       <= 5'd0;
            buf_len_r       <= 11'd0;
            buf_wdata_r     <= 8'd0;
            buf_wvalid_r    <= 1'b0;
            rx_rsr          <= 16'd0;
            rx_rd_ptr       <= 16'd0;
            tx_wr_ptr       <= 16'd0;
            tx_remaining    <= 16'd0;
            sock_status     <= 8'd0;
            sub_step        <= 2'd0;
        end else begin
            // Default: deassert one-cycle pulses
            reg_start_r  <= 1'b0;
            buf_start_r  <= 1'b0;
            buf_wvalid_r <= 1'b0;
            rx_valid     <= 1'b0;
            tx_done      <= 1'b0;

            case (state)

                // ==========================================================
                // PHASE 1: Software reset — write MR=0x80, wait
                // ==========================================================
                S_RESET_WAIT: begin
                    // Small delay before first SPI to let W5500 power up
                    if (wait_cnt == RESET_WAIT) begin
                        wait_cnt <= 20'd0;
                        init_idx <= 5'd0;
                        state    <= S_INIT_WRITE;
                    end else begin
                        wait_cnt <= wait_cnt + 20'd1;
                    end
                end

                // ==========================================================
                // PHASE 2: Init ROM — write configuration registers
                // ==========================================================
                S_INIT_WRITE: begin
                    // Issue write from init_rom[init_idx]
                    reg_addr_r  <= init_rom[init_idx][28:13];
                    reg_bsb_r   <= init_rom[init_idx][12:8];
                    reg_wdata_r <= init_rom[init_idx][7:0];
                    reg_write_r <= 1'b1;
                    reg_start_r <= 1'b1;
                    state       <= S_INIT_WAIT;
                end

                S_INIT_WAIT: begin
                    if (reg_done_w) begin
                        if (init_idx == 5'd0) begin
                            // After reset write, wait then verify MR=0x00
                            wait_cnt <= 20'd0;
                            state    <= S_RESET_VERIFY;
                        end else if (init_idx == (INIT_LEN - 1)) begin
                            // All init writes done → open socket
                            state <= S_SOCK_OPEN;
                        end else begin
                            init_idx <= init_idx + 5'd1;
                            state    <= S_INIT_WRITE;
                        end
                    end
                end

                // Wait after SW reset, then verify MR reads 0x00
                S_RESET_VERIFY: begin
                    if (wait_cnt == RESET_WAIT) begin
                        issue_reg_read(MR_ADDR, BSB_COMMON);
                        state <= S_RESET_CHECK;
                    end else begin
                        wait_cnt <= wait_cnt + 20'd1;
                    end
                end

                S_RESET_CHECK: begin
                    if (reg_done_w) begin
                        if (reg_rdata_w == 8'h00) begin
                            // Reset complete, continue with config
                            init_idx <= 5'd1;
                            state    <= S_INIT_WRITE;
                        end else begin
                            // Reset not complete — retry verify
                            wait_cnt <= 20'd0;
                            state    <= S_RESET_VERIFY;
                        end
                    end
                end

                // ==========================================================
                // PHASE 3: Socket 0 OPEN, poll for SOCK_INIT
                // ==========================================================
                S_SOCK_OPEN: begin
                    issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_OPEN);
                    state <= S_SOCK_OPEN_WAIT;
                end

                S_SOCK_OPEN_WAIT: begin
                    if (reg_done_w) begin
                        state <= S_POLL_INIT;
                    end
                end

                S_POLL_INIT: begin
                    issue_reg_read(SN_SR_ADDR, BSB_S0_REG);
                    state <= S_POLL_INIT_CHK;
                end

                S_POLL_INIT_CHK: begin
                    if (reg_done_w) begin
                        if (reg_rdata_w == SOCK_INIT)
                            state <= S_SOCK_LISTEN;
                        else
                            state <= S_POLL_INIT;  // keep polling
                    end
                end

                // ==========================================================
                // PHASE 4: Socket 0 LISTEN, poll for SOCK_LISTEN
                // ==========================================================
                S_SOCK_LISTEN: begin
                    issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_LISTEN);
                    state <= S_SOCK_LISN_WAIT;
                end

                S_SOCK_LISN_WAIT: begin
                    if (reg_done_w) begin
                        state <= S_POLL_LISTEN;
                    end
                end

                S_POLL_LISTEN: begin
                    issue_reg_read(SN_SR_ADDR, BSB_S0_REG);
                    state <= S_POLL_LISN_CHK;
                end

                S_POLL_LISN_CHK: begin
                    if (reg_done_w) begin
                        if (reg_rdata_w == SOCK_LISTEN) begin
                            init_done <= 1'b1;
                            state     <= S_WAIT_CONNECT;
                        end else begin
                            state <= S_POLL_LISTEN;
                        end
                    end
                end

                // ==========================================================
                // PHASE 5: Wait for client connection
                // ==========================================================
                S_WAIT_CONNECT: begin
                    client_connected <= 1'b0;
                    issue_reg_read(SN_SR_ADDR, BSB_S0_REG);
                    state <= S_CHK_CONNECT;
                end

                S_CHK_CONNECT: begin
                    if (reg_done_w) begin
                        sock_status <= reg_rdata_w;
                        case (reg_rdata_w)
                            SOCK_ESTABLISHED: begin
                                client_connected <= 1'b1;
                                if (!tx_active) begin
                                    // Check for incoming RX data
                                    state <= S_READ_RSR_H;
                                end else begin
                                    state <= S_ESTABLISHED;
                                end
                            end
                            SOCK_CLOSE_WAIT: begin
                                state <= S_DISCONNECT;
                            end
                            SOCK_CLOSED: begin
                                // Unexpected close — reopen
                                client_connected <= 1'b0;
                                state <= S_SOCK_OPEN;
                            end
                            default: begin
                                // Still listening — keep polling
                                state <= S_WAIT_CONNECT;
                            end
                        endcase
                    end
                end

                // ==========================================================
                // PHASE 6: Connection established — handle data
                // ==========================================================
                S_ESTABLISHED: begin
                    if (tx_start && !tx_active) begin
                        // TX request from HTTP parser
                        tx_remaining <= tx_len;
                        tx_active    <= 1'b1;
                        sub_step     <= 2'd0;
                        state        <= S_TX_READ_WR_H;
                    end else if (rx_consume) begin
                        // Parser consumed RX data — check for more
                        state <= S_READ_RSR_H;
                    end else begin
                        // Poll socket status for state changes or incoming data
                        issue_reg_read(SN_SR_ADDR, BSB_S0_REG);
                        state <= S_CHK_CONNECT;
                    end
                end

                // ==========================================================
                // RX PATH: Read Sn_RX_RSR, then read buffer
                // ==========================================================
                // Read RSR high byte
                S_READ_RSR_H: begin
                    issue_reg_read(SN_RX_RSR_ADDR, BSB_S0_REG);
                    state <= S_READ_RSR_L;
                end

                // RSR high done → read RSR low byte
                S_READ_RSR_L: begin
                    if (reg_done_w) begin
                        rx_rsr[15:8] <= reg_rdata_w;
                        issue_reg_read(SN_RX_RSR_ADDR + 16'd1, BSB_S0_REG);
                        state <= S_READ_RSR_CHK;
                    end
                end

                // RSR low done → check if data available
                S_READ_RSR_CHK: begin
                    if (reg_done_w) begin
                        rx_rsr[7:0] <= reg_rdata_w;
                        if ({rx_rsr[15:8], reg_rdata_w} == 16'd0) begin
                            // No data — return to established (poll again)
                            state <= S_ESTABLISHED;
                        end else begin
                            rx_len <= {rx_rsr[15:8], reg_rdata_w};
                            sub_step <= 2'd0;
                            state <= S_READ_RD_H;
                        end
                    end
                end

                // Read Sn_RX_RD pointer (high byte)
                S_READ_RD_H: begin
                    issue_reg_read(SN_RX_RD_ADDR, BSB_S0_REG);
                    state <= S_READ_RD_L;
                end

                S_READ_RD_L: begin
                    if (reg_done_w) begin
                        rx_rd_ptr[15:8] <= reg_rdata_w;
                        issue_reg_read(SN_RX_RD_ADDR + 16'd1, BSB_S0_REG);
                        state <= S_RX_BUF_READ;
                    end
                end

                // Start buffer read from RX buffer
                S_RX_BUF_READ: begin
                    if (reg_done_w) begin
                        rx_rd_ptr[7:0] <= reg_rdata_w;
                        // Issue buffer read: address = rx_rd_ptr, BSB = S0 RX
                        buf_addr_r  <= {rx_rd_ptr[15:8], reg_rdata_w};
                        buf_bsb_r   <= BSB_S0_RX;
                        buf_write_r <= 1'b0;
                        // Clamp to 11-bit buf_len (max 2047 per transfer)
                        buf_len_r   <= (rx_len > 16'd2047) ? 11'd2047 : rx_len[10:0];
                        buf_start_r <= 1'b1;
                        state       <= S_RX_UPDATE_RD;
                    end
                end

                // Wait for buffer read to complete, forward bytes via rx_valid
                S_RX_UPDATE_RD: begin
                    // Forward each received byte to HTTP parser
                    if (buf_rvalid_w) begin
                        rx_data  <= buf_rdata_w;
                        rx_valid <= 1'b1;
                    end
                    if (buf_done_w) begin
                        // Update Sn_RX_RD: advance by bytes read
                        // Calculate new read pointer
                        rx_rd_ptr <= rx_rd_ptr + ((rx_len > 16'd2047) ? 16'd2047 : rx_len);
                        sub_step  <= 2'd0;
                        state     <= S_RX_CMD_RECV;
                    end
                end

                // Write updated Sn_RX_RD, then issue RECV command
                S_RX_CMD_RECV: begin
                    case (sub_step)
                        2'd0: begin
                            // Write RX_RD high byte
                            issue_reg_write(SN_RX_RD_ADDR, BSB_S0_REG, rx_rd_ptr[15:8]);
                            sub_step <= 2'd1;
                        end
                        2'd1: begin
                            if (reg_done_w) begin
                                // Write RX_RD low byte
                                issue_reg_write(SN_RX_RD_ADDR + 16'd1, BSB_S0_REG, rx_rd_ptr[7:0]);
                                sub_step <= 2'd2;
                            end
                        end
                        2'd2: begin
                            if (reg_done_w) begin
                                // Issue RECV command
                                issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_RECV);
                                sub_step <= 2'd3;
                            end
                        end
                        2'd3: begin
                            if (reg_done_w) begin
                                // RX complete — back to established
                                state <= S_ESTABLISHED;
                            end
                        end
                    endcase
                end

                // ==========================================================
                // TX PATH: Read Sn_TX_WR, write buffer, issue SEND
                // ==========================================================
                S_TX_READ_WR_H: begin
                    issue_reg_read(SN_TX_WR_ADDR, BSB_S0_REG);
                    state <= S_TX_READ_WR_L;
                end

                S_TX_READ_WR_L: begin
                    if (reg_done_w) begin
                        tx_wr_ptr[15:8] <= reg_rdata_w;
                        issue_reg_read(SN_TX_WR_ADDR + 16'd1, BSB_S0_REG);
                        state <= S_TX_BUF_WRITE;
                    end
                end

                // Start buffer write to TX buffer
                S_TX_BUF_WRITE: begin
                    if (reg_done_w) begin
                        tx_wr_ptr[7:0] <= reg_rdata_w;
                        buf_addr_r  <= {tx_wr_ptr[15:8], reg_rdata_w};
                        buf_bsb_r   <= BSB_S0_TX;
                        buf_write_r <= 1'b1;
                        buf_len_r   <= (tx_remaining > 16'd2047) ? 11'd2047 : tx_remaining[10:0];
                        buf_start_r <= 1'b1;
                        state       <= S_TX_UPDATE_WR;
                    end
                end

                // Feed TX bytes and wait for buffer write to complete
                S_TX_UPDATE_WR: begin
                    // Drive buf_wdata from tx_data, assert wvalid when tx_valid
                    buf_wdata_r  <= tx_data;
                    buf_wvalid_r <= tx_valid;

                    if (buf_done_w) begin
                        // Update TX write pointer
                        tx_wr_ptr <= tx_wr_ptr + ((tx_remaining > 16'd2047) ? 16'd2047 : tx_remaining);
                        sub_step  <= 2'd0;
                        state     <= S_TX_CMD_SEND;
                    end
                end

                // Write updated Sn_TX_WR, then issue SEND command
                S_TX_CMD_SEND: begin
                    case (sub_step)
                        2'd0: begin
                            issue_reg_write(SN_TX_WR_ADDR, BSB_S0_REG, tx_wr_ptr[15:8]);
                            sub_step <= 2'd1;
                        end
                        2'd1: begin
                            if (reg_done_w) begin
                                issue_reg_write(SN_TX_WR_ADDR + 16'd1, BSB_S0_REG, tx_wr_ptr[7:0]);
                                sub_step <= 2'd2;
                            end
                        end
                        2'd2: begin
                            if (reg_done_w) begin
                                issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_SEND);
                                sub_step <= 2'd3;
                            end
                        end
                        2'd3: begin
                            if (reg_done_w) begin
                                tx_active <= 1'b0;
                                tx_done   <= 1'b1;
                                state     <= S_ESTABLISHED;
                            end
                        end
                    endcase
                end

                // ==========================================================
                // CLOSE / DISCONNECT: reopen socket
                // ==========================================================
                S_DISCONNECT: begin
                    client_connected <= 1'b0;
                    issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_DISCON);
                    state <= S_DISC_WAIT;
                end

                S_DISC_WAIT: begin
                    if (reg_done_w) begin
                        state <= S_CLOSE_CMD;
                    end
                end

                S_CLOSE_CMD: begin
                    if (sub_step == 2'd0) begin
                        // Issue CLOSE command
                        issue_reg_write(SN_CR_ADDR, BSB_S0_REG, CMD_CLOSE);
                        sub_step <= 2'd1;
                    end else begin
                        if (reg_done_w) begin
                            // CLOSE done — reopen socket from scratch
                            sub_step <= 2'd0;
                            state    <= S_SOCK_OPEN;
                        end
                    end
                end

                default: begin
                    state <= S_RESET_WAIT;
                end
            endcase
        end
    end

endmodule
