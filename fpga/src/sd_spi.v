`timescale 1ns / 1ps
//============================================================================
// sd_spi.v — Top-level SD card SPI controller for iCE40 HX8K
//
// Integrates sd_init, sd_read, sd_write with a shared spi_master and CRC
// engines. Multiplexes SPI control signals based on the active operation.
// Auto-initializes after reset; then dispatches read/write via cmd_start.
//============================================================================

module sd_spi (
    input  wire        clk,
    input  wire        rst_n,
    // Command interface (from KV engine)
    input  wire        cmd_start,
    input  wire [1:0]  cmd_op,       // 00=init, 01=read, 10=write
    input  wire [31:0] cmd_block_addr,
    input  wire [7:0]  write_data,
    input  wire        write_valid,
    output wire [7:0]  read_data,
    output wire        read_valid,
    output wire        cmd_done,
    output wire        cmd_error,
    output wire        card_ready,
    output wire        card_sdhc,
    output wire        write_req,
    output wire        block_done,
    // SPI pins
    output wire        sd_cs_n,
    output wire        sd_sck,
    output wire        sd_mosi,
    input  wire        sd_miso
);

    // ── Operation select ────────────────────────────────────────────────
    localparam [1:0]
        OP_INIT  = 2'b00,
        OP_READ  = 2'b01,
        OP_WRITE = 2'b10;

    // Active operation tracking
    localparam [1:0]
        ACT_NONE  = 2'b00,
        ACT_INIT  = 2'b01,
        ACT_READ  = 2'b10,
        ACT_WRITE = 2'b11;

    reg [1:0] active_op;

    // ── SPI master signals ──────────────────────────────────────────────
    wire       spi_start_mux;
    wire [7:0] spi_tx_data_mux;
    wire       spi_cs_assert_mux;
    wire [7:0] spi_rx_data;
    wire       spi_done;

    // ── Sub-module SPI signals ──────────────────────────────────────────
    // Init
    wire       init_spi_start;
    wire [7:0] init_spi_tx_data;
    wire       init_spi_cs_assert;
    wire       init_ready;
    wire       init_card_sdhc;
    wire       init_error;
    wire       init_busy;

    // Read
    wire       rd_spi_start;
    wire [7:0] rd_spi_tx_data;
    wire       rd_spi_cs_assert;
    wire       rd_crc_clear;
    wire       rd_crc_enable;
    wire       rd_crc_data_in;
    wire [7:0] rd_data_out;
    wire       rd_data_valid;
    wire       rd_block_done;
    wire       rd_error;
    wire       rd_busy;

    // Write
    wire       wr_spi_start;
    wire [7:0] wr_spi_tx_data;
    wire       wr_spi_cs_assert;
    wire       wr_crc_clear;
    wire       wr_crc_enable;
    wire       wr_crc_data_in;
    wire       wr_data_req;
    wire       wr_block_done;
    wire       wr_error;
    wire       wr_busy;

    // ── CRC16 signals ───────────────────────────────────────────────────
    wire        crc16_clear;
    wire        crc16_enable;
    wire        crc16_data_in;
    wire [15:0] crc16_out;

    // ── SPI mux ─────────────────────────────────────────────────────────
    assign spi_start_mux    = (active_op == ACT_INIT)  ? init_spi_start    :
                              (active_op == ACT_READ)  ? rd_spi_start      :
                              (active_op == ACT_WRITE) ? wr_spi_start      :
                              1'b0;

    assign spi_tx_data_mux  = (active_op == ACT_INIT)  ? init_spi_tx_data  :
                              (active_op == ACT_READ)  ? rd_spi_tx_data    :
                              (active_op == ACT_WRITE) ? wr_spi_tx_data    :
                              8'hFF;

    assign spi_cs_assert_mux = (active_op == ACT_INIT)  ? init_spi_cs_assert :
                               (active_op == ACT_READ)  ? rd_spi_cs_assert   :
                               (active_op == ACT_WRITE) ? wr_spi_cs_assert   :
                               1'b0;

    // ── CRC16 mux ───────────────────────────────────────────────────────
    assign crc16_clear   = (active_op == ACT_READ)  ? rd_crc_clear   :
                           (active_op == ACT_WRITE) ? wr_crc_clear   :
                           1'b0;

    assign crc16_enable  = (active_op == ACT_READ)  ? rd_crc_enable  :
                           (active_op == ACT_WRITE) ? wr_crc_enable  :
                           1'b0;

    assign crc16_data_in = (active_op == ACT_READ)  ? rd_crc_data_in :
                           (active_op == ACT_WRITE) ? wr_crc_data_in :
                           1'b0;

    // ── Output assignments ──────────────────────────────────────────────
    assign read_data  = rd_data_out;
    assign read_valid = rd_data_valid;
    assign card_ready = init_ready;
    assign card_sdhc  = init_card_sdhc;
    assign write_req  = wr_data_req;

    assign block_done = (active_op == ACT_READ)  ? rd_block_done :
                        (active_op == ACT_WRITE) ? wr_block_done :
                        1'b0;

    assign cmd_error  = (active_op == ACT_INIT)  ? init_error :
                        (active_op == ACT_READ)  ? rd_error   :
                        (active_op == ACT_WRITE) ? wr_error   :
                        1'b0;

    // ── Start signals (directly driven from controller FSM) ─────────────
    reg init_start_r;
    reg rd_start_r;
    reg wr_start_r;
    reg rd_stop_r;
    reg rd_multi_r;
    reg wr_multi_r;
    reg wr_stop_r;
    reg [31:0] block_addr_r;

    // cmd_done: when the active sub-module finishes
    reg cmd_done_r;
    assign cmd_done = cmd_done_r;

    // ── Auto-init + dispatch FSM ────────────────────────────────────────
    localparam [2:0]
        C_RESET     = 3'd0,
        C_AUTO_INIT = 3'd1,
        C_IDLE      = 3'd2,
        C_RUNNING   = 3'd3;

    reg [2:0] ctrl_state;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ctrl_state   <= C_RESET;
            active_op    <= ACT_NONE;
            init_start_r <= 1'b0;
            rd_start_r   <= 1'b0;
            wr_start_r   <= 1'b0;
            rd_stop_r    <= 1'b0;
            rd_multi_r   <= 1'b0;
            wr_multi_r   <= 1'b0;
            wr_stop_r    <= 1'b0;
            block_addr_r <= 32'd0;
            cmd_done_r   <= 1'b0;
        end else begin
            // Defaults
            init_start_r <= 1'b0;
            rd_start_r   <= 1'b0;
            wr_start_r   <= 1'b0;
            rd_stop_r    <= 1'b0;
            wr_stop_r    <= 1'b0;
            cmd_done_r   <= 1'b0;

            case (ctrl_state)

            C_RESET: begin
                // Trigger auto-init one cycle after reset release
                active_op    <= ACT_INIT;
                init_start_r <= 1'b1;
                ctrl_state   <= C_AUTO_INIT;
            end

            C_AUTO_INIT: begin
                if (init_error) begin
                    active_op  <= ACT_NONE;
                    cmd_done_r <= 1'b1;
                    ctrl_state <= C_IDLE;
                end else if (init_ready) begin
                    active_op  <= ACT_NONE;
                    ctrl_state <= C_IDLE;
                end
            end

            C_IDLE: begin
                if (cmd_start) begin
                    block_addr_r <= cmd_block_addr;
                    case (cmd_op)
                        OP_INIT: begin
                            active_op    <= ACT_INIT;
                            init_start_r <= 1'b1;
                            ctrl_state   <= C_RUNNING;
                        end
                        OP_READ: begin
                            active_op  <= ACT_READ;
                            rd_start_r <= 1'b1;
                            rd_multi_r <= 1'b0;
                            ctrl_state <= C_RUNNING;
                        end
                        OP_WRITE: begin
                            active_op  <= ACT_WRITE;
                            wr_start_r <= 1'b1;
                            wr_multi_r <= 1'b0;
                            ctrl_state <= C_RUNNING;
                        end
                        default: ;
                    endcase
                end
            end

            C_RUNNING: begin
                case (active_op)
                    ACT_INIT: begin
                        if (!init_busy && !init_start_r) begin
                            cmd_done_r <= 1'b1;
                            active_op  <= ACT_NONE;
                            ctrl_state <= C_IDLE;
                        end
                    end
                    ACT_READ: begin
                        if (!rd_busy && !rd_start_r) begin
                            cmd_done_r <= 1'b1;
                            active_op  <= ACT_NONE;
                            ctrl_state <= C_IDLE;
                        end
                    end
                    ACT_WRITE: begin
                        if (!wr_busy && !wr_start_r) begin
                            cmd_done_r <= 1'b1;
                            active_op  <= ACT_NONE;
                            ctrl_state <= C_IDLE;
                        end
                    end
                    default: begin
                        ctrl_state <= C_IDLE;
                    end
                endcase
            end

            default: ctrl_state <= C_RESET;

            endcase
        end
    end

    // ── SPI Master instance ─────────────────────────────────────────────
    spi_master #(
        .CLK_DIV(4)
    ) u_spi (
        .clk       (clk),
        .rst_n     (rst_n),
        .start     (spi_start_mux),
        .tx_data   (spi_tx_data_mux),
        .rx_data   (spi_rx_data),
        .done      (spi_done),
        .cs_assert (spi_cs_assert_mux),
        .spi_clk   (sd_sck),
        .spi_mosi  (sd_mosi),
        .spi_miso  (sd_miso),
        .spi_cs_n  (sd_cs_n)
    );

    // ── CRC7 instance (available for future command CRC use) ────────────
    // Not actively driven in this integration; wired for expansion.
    wire       crc7_clear  = 1'b0;
    wire       crc7_enable = 1'b0;
    wire       crc7_data_in = 1'b0;
    wire [6:0] crc7_out;

    crc7 u_crc7 (
        .clk     (clk),
        .rst_n   (rst_n),
        .clear   (crc7_clear),
        .enable  (crc7_enable),
        .data_in (crc7_data_in),
        .crc_out (crc7_out)
    );

    // ── CRC16 instance ──────────────────────────────────────────────────
    crc16 u_crc16 (
        .clk     (clk),
        .rst_n   (rst_n),
        .clear   (crc16_clear),
        .enable  (crc16_enable),
        .data_in (crc16_data_in),
        .crc_out (crc16_out)
    );

    // ── SD Init ─────────────────────────────────────────────────────────
    sd_init u_init (
        .clk          (clk),
        .rst_n        (rst_n),
        .start        (init_start_r),
        .spi_start    (init_spi_start),
        .spi_tx_data  (init_spi_tx_data),
        .spi_cs_assert(init_spi_cs_assert),
        .spi_rx_data  (spi_rx_data),
        .spi_done     (spi_done),
        .ready        (init_ready),
        .card_sdhc    (init_card_sdhc),
        .error        (init_error),
        .busy         (init_busy)
    );

    // ── SD Read ─────────────────────────────────────────────────────────
    sd_read u_read (
        .clk          (clk),
        .rst_n        (rst_n),
        .start        (rd_start_r),
        .block_addr   (block_addr_r),
        .multi        (rd_multi_r),
        .stop         (rd_stop_r),
        .spi_start    (rd_spi_start),
        .spi_tx_data  (rd_spi_tx_data),
        .spi_cs_assert(rd_spi_cs_assert),
        .spi_rx_data  (spi_rx_data),
        .spi_done     (spi_done),
        .crc_clear    (rd_crc_clear),
        .crc_enable   (rd_crc_enable),
        .crc_data_in  (rd_crc_data_in),
        .crc_out      (crc16_out),
        .data_out     (rd_data_out),
        .data_valid   (rd_data_valid),
        .block_done   (rd_block_done),
        .error        (rd_error),
        .busy         (rd_busy)
    );

    // ── SD Write ────────────────────────────────────────────────────────
    sd_write u_write (
        .clk          (clk),
        .rst_n        (rst_n),
        .start        (wr_start_r),
        .block_addr   (block_addr_r),
        .multi        (wr_multi_r),
        .stop         (wr_stop_r),
        .data_in      (write_data),
        .data_wr      (write_valid),
        .spi_start    (wr_spi_start),
        .spi_tx_data  (wr_spi_tx_data),
        .spi_cs_assert(wr_spi_cs_assert),
        .spi_rx_data  (spi_rx_data),
        .spi_done     (spi_done),
        .crc_clear    (wr_crc_clear),
        .crc_enable   (wr_crc_enable),
        .crc_data_in  (wr_crc_data_in),
        .crc_out      (crc16_out),
        .data_req     (wr_data_req),
        .block_done   (wr_block_done),
        .error        (wr_error),
        .busy         (wr_busy)
    );

endmodule
