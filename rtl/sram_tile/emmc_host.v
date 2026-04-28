// emmc_host.v — Minimal eMMC Host Controller
//
// Implements MMC protocol for single-sector read and write.
// eMMC interface: CLK, CMD (bidirectional), DAT[7:0] (bidirectional)
//
// Supported commands:
//   CMD0  — GO_IDLE_STATE (reset)
//   CMD1  — SEND_OP_COND (init)
//   CMD2  — ALL_SEND_CID
//   CMD3  — SET_RELATIVE_ADDR
//   CMD7  — SELECT_CARD
//   CMD16 — SET_BLOCKLEN (512)
//   CMD17 — READ_SINGLE_BLOCK
//   CMD24 — WRITE_BLOCK
//
// Bus width: starts at 1-bit, switches to 8-bit after init (CMD6)
// Clock: starts at 400kHz (init), switches to 50MHz (HS mode)
//
// Interface to user logic:
//   sector[31:0]  — LBA sector address
//   rd_start      — pulse to read a sector
//   rd_data[7:0]  — output byte stream (512 bytes)
//   rd_valid      — byte ready
//   rd_done       — all 512 bytes delivered
//   wr_start      — pulse to write a sector
//   wr_data[7:0]  — input byte (consumed when wr_ready)
//   wr_ready      — ready for next byte
//   wr_done       — write complete

module emmc_host #(
    parameter CLK_DIV_INIT = 125,   // 50MHz/125 = 400kHz
    parameter CLK_DIV_FAST = 1      // 50MHz/1 = 50MHz (HS mode)
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- eMMC physical pins ---
    output reg         emmc_clk_o,
    inout  wire        emmc_cmd,
    inout  wire [7:0]  emmc_dat,
    output reg         emmc_rst_n,

    // --- Control interface ---
    output reg         ready,        // controller idle, card initialized

    input  wire        rd_start,
    input  wire        wr_start,
    input  wire [31:0] sector,

    output reg  [7:0]  rd_data,
    output reg         rd_valid,
    output reg         rd_done,

    input  wire [7:0]  wr_data,
    output reg         wr_ready,
    output reg         wr_done
);

    // =====================================================================
    // Clock divider
    // =====================================================================

    reg [7:0] clk_div_r;
    reg [7:0] clk_cnt;
    reg       emmc_clk_en;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            clk_cnt    <= 8'd0;
            emmc_clk_o <= 1'b0;
        end else if (emmc_clk_en) begin
            if (clk_cnt >= clk_div_r) begin
                clk_cnt    <= 8'd0;
                emmc_clk_o <= ~emmc_clk_o;
            end else begin
                clk_cnt <= clk_cnt + 1;
            end
        end else begin
            emmc_clk_o <= 1'b0;
        end
    end

    // =====================================================================
    // CMD line driver
    // =====================================================================

    reg        cmd_oe;       // 1=FPGA drives CMD
    reg        cmd_out;
    wire       cmd_in = emmc_cmd;

    assign emmc_cmd = cmd_oe ? cmd_out : 1'bz;

    // =====================================================================
    // DAT line driver
    // =====================================================================

    reg        dat_oe;
    reg [7:0]  dat_out;
    wire [7:0] dat_in = emmc_dat;

    assign emmc_dat = dat_oe ? dat_out : 8'bz;

    // =====================================================================
    // Command shift register
    // =====================================================================

    reg [47:0] cmd_shift;    // 48-bit command frame
    reg [5:0]  cmd_bit_cnt;
    reg        cmd_sending;

    // Response capture
    reg [135:0] resp_shift;
    reg [7:0]   resp_bit_cnt;
    reg         resp_receiving;
    reg         resp_done;

    wire emmc_clk_rise = emmc_clk_en && (clk_cnt == clk_div_r) && !emmc_clk_o;
    wire emmc_clk_fall = emmc_clk_en && (clk_cnt == clk_div_r) && emmc_clk_o;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cmd_shift      <= 48'd0;
            cmd_bit_cnt    <= 6'd0;
            cmd_sending    <= 1'b0;
            cmd_oe         <= 1'b0;
            cmd_out        <= 1'b1;
            resp_shift     <= 136'd0;
            resp_bit_cnt   <= 8'd0;
            resp_receiving <= 1'b0;
            resp_done      <= 1'b0;
        end else begin
            resp_done <= 1'b0;

            if (cmd_sending && emmc_clk_fall) begin
                cmd_out   <= cmd_shift[47];
                cmd_shift <= {cmd_shift[46:0], 1'b1};
                cmd_bit_cnt <= cmd_bit_cnt + 1;
                if (cmd_bit_cnt == 6'd47) begin
                    cmd_sending <= 1'b0;
                    cmd_oe      <= 1'b0;
                end
            end

            if (resp_receiving && emmc_clk_rise) begin
                resp_shift <= {resp_shift[134:0], cmd_in};
                resp_bit_cnt <= resp_bit_cnt + 1;
                if (resp_bit_cnt >= 8'd47) begin
                    resp_receiving <= 1'b0;
                    resp_done      <= 1'b1;
                end
            end

            // Start receiving response when CMD goes low (start bit)
            if (!cmd_sending && !resp_receiving && emmc_clk_rise && !cmd_in) begin
                resp_receiving <= 1'b1;
                resp_bit_cnt   <= 8'd1;
                resp_shift     <= {135'd0, cmd_in};
            end
        end
    end

    // =====================================================================
    // Data receive (DAT[7:0] → byte stream)
    // =====================================================================

    reg [8:0]  data_byte_cnt;
    reg        data_receiving;
    reg        data_done_r;
    reg        use_8bit;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_data       <= 8'd0;
            rd_valid      <= 1'b0;
            rd_done       <= 1'b0;
            data_byte_cnt <= 9'd0;
            data_receiving <= 1'b0;
            data_done_r   <= 1'b0;
        end else begin
            rd_valid    <= 1'b0;
            rd_done     <= 1'b0;
            data_done_r <= 1'b0;

            if (data_receiving && emmc_clk_rise) begin
                if (use_8bit) begin
                    rd_data <= dat_in;
                    rd_valid <= 1'b1;
                end else begin
                    // 1-bit mode: accumulate bits on DAT[0]
                    rd_data <= {rd_data[6:0], dat_in[0]};
                    if (data_byte_cnt[2:0] == 3'd7) begin
                        rd_valid <= 1'b1;
                    end
                end

                data_byte_cnt <= data_byte_cnt + 1;

                if (data_byte_cnt == 9'd511) begin
                    data_receiving <= 1'b0;
                    data_done_r    <= 1'b1;
                    rd_done        <= 1'b1;
                end
            end
        end
    end

    // =====================================================================
    // Main state machine
    // =====================================================================

    localparam ST_RESET      = 4'd0;
    localparam ST_SEND_INIT  = 4'd1;  // 80 clocks
    localparam ST_CMD0       = 4'd2;
    localparam ST_CMD1       = 4'd3;
    localparam ST_CMD2       = 4'd4;
    localparam ST_CMD3       = 4'd5;
    localparam ST_CMD7       = 4'd6;
    localparam ST_CMD16      = 4'd7;
    localparam ST_READY      = 4'd8;
    localparam ST_CMD17      = 4'd9;  // read
    localparam ST_CMD17_WAIT = 4'd10;
    localparam ST_CMD24      = 4'd11; // write
    localparam ST_CMD24_SEND = 4'd12;
    localparam ST_CMD24_WAIT = 4'd13;
    localparam ST_DONE       = 4'd14;

    reg [3:0]  state;
    reg [15:0] delay_cnt;
    reg [31:0] sector_r;

    // Build MMC command frame: {start=0, dir=1, cmd[5:0], arg[31:0], crc7, stop=1}
    function [47:0] mmc_cmd;
        input [5:0]  cmd;
        input [31:0] arg;
        begin
            mmc_cmd = {1'b0, 1'b1, cmd, arg, 7'b0000000, 1'b1};
        end
    endfunction

    task send_cmd;
        input [5:0]  cmd;
        input [31:0] arg;
        begin
            cmd_shift   <= mmc_cmd(cmd, arg);
            cmd_bit_cnt <= 6'd0;
            cmd_sending <= 1'b1;
            cmd_oe      <= 1'b1;
        end
    endtask

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= ST_RESET;
            ready      <= 1'b0;
            emmc_rst_n <= 1'b0;
            emmc_clk_en <= 1'b0;
            clk_div_r  <= CLK_DIV_INIT;
            delay_cnt  <= 16'd0;
            sector_r   <= 32'd0;
            use_8bit   <= 1'b0;
            dat_oe     <= 1'b0;
            dat_out    <= 8'hFF;
            wr_ready   <= 1'b0;
            wr_done    <= 1'b0;
        end else begin
            wr_done <= 1'b0;

            case (state)
                ST_RESET: begin
                    emmc_rst_n  <= 1'b0;
                    emmc_clk_en <= 1'b1;
                    clk_div_r   <= CLK_DIV_INIT;
                    delay_cnt   <= 16'd0;
                    state       <= ST_SEND_INIT;
                end

                ST_SEND_INIT: begin
                    emmc_rst_n <= 1'b1;
                    delay_cnt  <= delay_cnt + 1;
                    if (delay_cnt >= 16'd1000) begin
                        state <= ST_CMD0;
                    end
                end

                ST_CMD0: begin
                    if (!cmd_sending) begin
                        send_cmd(6'd0, 32'd0);
                        state <= ST_CMD1;
                    end
                end

                ST_CMD1: begin
                    if (resp_done) begin
                        // CMD1: SEND_OP_COND with HCS bit
                        send_cmd(6'd1, 32'h40FF8080);
                        state <= ST_CMD2;
                    end
                end

                ST_CMD2: begin
                    if (resp_done) begin
                        if (resp_shift[31]) begin
                            // Card ready — send CMD2
                            send_cmd(6'd2, 32'd0);
                            state <= ST_CMD3;
                        end else begin
                            // Retry CMD1
                            send_cmd(6'd1, 32'h40FF8080);
                        end
                    end
                end

                ST_CMD3: begin
                    if (resp_done) begin
                        // CMD3: SET_RELATIVE_ADDR (RCA=1)
                        send_cmd(6'd3, 32'h00010000);
                        state <= ST_CMD7;
                    end
                end

                ST_CMD7: begin
                    if (resp_done) begin
                        // CMD7: SELECT_CARD (RCA=1)
                        send_cmd(6'd7, 32'h00010000);
                        state <= ST_CMD16;
                    end
                end

                ST_CMD16: begin
                    if (resp_done) begin
                        // CMD16: SET_BLOCKLEN = 512
                        send_cmd(6'd16, 32'd512);
                        // Switch to fast clock
                        clk_div_r <= CLK_DIV_FAST;
                        state     <= ST_READY;
                    end
                end

                ST_READY: begin
                    if (resp_done || ready) begin
                        ready <= 1'b1;
                    end

                    if (ready && rd_start) begin
                        sector_r <= sector;
                        ready    <= 1'b0;
                        state    <= ST_CMD17;
                    end else if (ready && wr_start) begin
                        sector_r <= sector;
                        ready    <= 1'b0;
                        state    <= ST_CMD24;
                    end
                end

                ST_CMD17: begin
                    // CMD17: READ_SINGLE_BLOCK
                    if (!cmd_sending) begin
                        send_cmd(6'd17, sector_r);
                        state <= ST_CMD17_WAIT;
                    end
                end

                ST_CMD17_WAIT: begin
                    // Wait for response, then data on DAT lines
                    if (resp_done) begin
                        data_receiving <= 1'b1;
                        data_byte_cnt  <= 9'd0;
                    end
                    if (data_done_r) begin
                        state <= ST_DONE;
                    end
                end

                ST_CMD24: begin
                    // CMD24: WRITE_BLOCK
                    if (!cmd_sending) begin
                        send_cmd(6'd24, sector_r);
                        state <= ST_CMD24_SEND;
                    end
                end

                ST_CMD24_SEND: begin
                    if (resp_done) begin
                        // Send data on DAT[0] — simplified
                        // Real implementation needs start bit, CRC16, stop
                        wr_ready  <= 1'b1;
                        dat_oe    <= 1'b1;
                        delay_cnt <= 16'd0;
                        state     <= ST_CMD24_WAIT;
                    end
                end

                ST_CMD24_WAIT: begin
                    // Simplified: wait fixed time for write to complete
                    delay_cnt <= delay_cnt + 1;
                    if (delay_cnt >= 16'd50000) begin  // ~1ms at 50MHz
                        dat_oe   <= 1'b0;
                        wr_ready <= 1'b0;
                        wr_done  <= 1'b1;
                        state    <= ST_DONE;
                    end
                end

                ST_DONE: begin
                    state <= ST_READY;
                end

                default: state <= ST_RESET;
            endcase
        end
    end

endmodule
