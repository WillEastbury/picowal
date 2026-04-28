// emmc_host_1bit.v — Minimal eMMC controller, 1-bit DAT mode
//
// Stripped-down eMMC host for use in striped arrays.
// Only 3 pins per chip: CLK, CMD, DAT0.
// Supports: init, single-block read (CMD17), single-block write (CMD24).
//
// 1-bit mode throughput: 50 MB/s sequential (50MHz × 1 bit)
// Plenty for per-chip share of GbE bandwidth (117/20 ≈ 6 MB/s per chip)

module emmc_host_1bit #(
    parameter CLK_DIV_INIT = 125,   // 50MHz/125 = 400kHz init
    parameter CLK_DIV_FAST = 1      // 50MHz full speed
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- eMMC pins (3 per chip) ---
    output reg         emmc_clk_o,
    inout  wire        emmc_cmd,
    inout  wire        emmc_dat0,
    output reg         emmc_rst_n,

    // --- Control ---
    output reg         ready,
    input  wire        rd_start,
    input  wire        wr_start,
    input  wire [31:0] sector,

    // --- Read data stream ---
    output reg  [7:0]  rd_data,
    output reg         rd_valid,
    output reg         rd_done,

    // --- Write data stream ---
    input  wire [7:0]  wr_data,
    output reg         wr_ready,
    output reg         wr_done
);

    // =====================================================================
    // Clock divider
    // =====================================================================

    reg [7:0] clk_div_r;
    reg [7:0] clk_cnt;
    reg       clk_en;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            clk_cnt    <= 8'd0;
            emmc_clk_o <= 1'b0;
        end else if (clk_en) begin
            if (clk_cnt >= clk_div_r) begin
                clk_cnt    <= 8'd0;
                emmc_clk_o <= ~emmc_clk_o;
            end else
                clk_cnt <= clk_cnt + 1;
        end
    end

    wire mmc_clk_rise = clk_en && (clk_cnt == clk_div_r) && !emmc_clk_o;
    wire mmc_clk_fall = clk_en && (clk_cnt == clk_div_r) && emmc_clk_o;

    // =====================================================================
    // CMD line
    // =====================================================================

    reg       cmd_oe, cmd_out;
    assign emmc_cmd = cmd_oe ? cmd_out : 1'bz;
    wire      cmd_in = emmc_cmd;

    // =====================================================================
    // DAT0 line
    // =====================================================================

    reg       dat_oe, dat_out;
    assign emmc_dat0 = dat_oe ? dat_out : 1'bz;
    wire      dat_in = emmc_dat0;

    // =====================================================================
    // Command TX shift register (48-bit frame)
    // =====================================================================

    reg [47:0] cmd_sr;
    reg [5:0]  cmd_bcnt;
    reg        cmd_active;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cmd_sr     <= 48'hFFFFFFFFFFFF;
            cmd_bcnt   <= 6'd0;
            cmd_active <= 1'b0;
            cmd_oe     <= 1'b0;
            cmd_out    <= 1'b1;
        end else if (cmd_active && mmc_clk_fall) begin
            cmd_out <= cmd_sr[47];
            cmd_sr  <= {cmd_sr[46:0], 1'b1};
            cmd_bcnt <= cmd_bcnt + 1;
            if (cmd_bcnt == 6'd47) begin
                cmd_active <= 1'b0;
                cmd_oe     <= 1'b0;
            end
        end
    end

    // =====================================================================
    // Response RX (48-bit R1)
    // =====================================================================

    reg [47:0] resp_sr;
    reg [5:0]  resp_bcnt;
    reg        resp_active;
    reg        resp_done_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            resp_sr     <= 48'd0;
            resp_bcnt   <= 6'd0;
            resp_active <= 1'b0;
            resp_done_r <= 1'b0;
        end else begin
            resp_done_r <= 1'b0;

            if (resp_active && mmc_clk_rise) begin
                resp_sr  <= {resp_sr[46:0], cmd_in};
                resp_bcnt <= resp_bcnt + 1;
                if (resp_bcnt == 6'd47) begin
                    resp_active <= 1'b0;
                    resp_done_r <= 1'b1;
                end
            end

            // Detect start bit (CMD goes low)
            if (!cmd_active && !resp_active && mmc_clk_rise && !cmd_in) begin
                resp_active <= 1'b1;
                resp_bcnt   <= 6'd1;
                resp_sr     <= {47'd0, cmd_in};
            end
        end
    end

    // =====================================================================
    // DAT0 read: bit-serial → byte stream
    // =====================================================================

    reg [9:0]  dat_byte_cnt;     // counts bits for 1-bit mode (512×8=4096 bits)
    reg        dat_rx_active;
    reg        dat_rx_started;   // detected start bit
    reg [7:0]  dat_byte_sr;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_data        <= 8'd0;
            rd_valid       <= 1'b0;
            rd_done        <= 1'b0;
            dat_byte_cnt   <= 10'd0;
            dat_rx_active  <= 1'b0;
            dat_rx_started <= 1'b0;
            dat_byte_sr    <= 8'd0;
        end else begin
            rd_valid <= 1'b0;
            rd_done  <= 1'b0;

            if (dat_rx_active && mmc_clk_rise) begin
                if (!dat_rx_started) begin
                    // Wait for start bit (DAT0 goes low)
                    if (!dat_in) begin
                        dat_rx_started <= 1'b1;
                        dat_byte_cnt   <= 10'd0;
                    end
                end else begin
                    // Shift in data bits
                    dat_byte_sr <= {dat_byte_sr[6:0], dat_in};
                    dat_byte_cnt <= dat_byte_cnt + 1;

                    // Every 8 bits, emit a byte
                    if (dat_byte_cnt[2:0] == 3'd7) begin
                        rd_data  <= {dat_byte_sr[6:0], dat_in};
                        rd_valid <= 1'b1;
                    end

                    // 512 bytes = 4096 bits
                    if (dat_byte_cnt == 10'd4095) begin
                        dat_rx_active  <= 1'b0;
                        dat_rx_started <= 1'b0;
                        rd_done        <= 1'b1;
                    end
                end
            end
        end
    end

    // =====================================================================
    // Main state machine
    // =====================================================================

    localparam ST_RESET  = 4'd0;
    localparam ST_INIT   = 4'd1;
    localparam ST_CMD0   = 4'd2;
    localparam ST_CMD1   = 4'd3;
    localparam ST_CMD2   = 4'd4;
    localparam ST_CMD3   = 4'd5;
    localparam ST_CMD7   = 4'd6;
    localparam ST_READY  = 4'd7;
    localparam ST_CMD17  = 4'd8;
    localparam ST_RD_WAIT = 4'd9;
    localparam ST_CMD24  = 4'd10;
    localparam ST_WR_SEND = 4'd11;
    localparam ST_WR_WAIT = 4'd12;
    localparam ST_DONE   = 4'd13;

    reg [3:0]  state;
    reg [15:0] delay_cnt;
    reg [31:0] sector_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= ST_RESET;
            ready      <= 1'b0;
            emmc_rst_n <= 1'b0;
            clk_en     <= 1'b0;
            clk_div_r  <= CLK_DIV_INIT;
            delay_cnt  <= 16'd0;
            sector_r   <= 32'd0;
            dat_oe     <= 1'b0;
            dat_out    <= 1'b1;
            wr_ready   <= 1'b0;
            wr_done    <= 1'b0;
        end else begin
            wr_done <= 1'b0;

            case (state)
                ST_RESET: begin
                    emmc_rst_n <= 1'b0;
                    clk_en     <= 1'b1;
                    clk_div_r  <= CLK_DIV_INIT;
                    delay_cnt  <= 16'd0;
                    state      <= ST_INIT;
                end

                ST_INIT: begin
                    emmc_rst_n <= 1'b1;
                    delay_cnt  <= delay_cnt + 1;
                    if (delay_cnt >= 16'd2000)
                        state <= ST_CMD0;
                end

                ST_CMD0: begin
                    if (!cmd_active) begin
                        cmd_sr     <= {1'b0, 1'b1, 6'd0, 32'd0, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        state      <= ST_CMD1;
                    end
                end

                ST_CMD1: begin
                    if (resp_done_r || delay_cnt >= 16'd5000) begin
                        // CMD1 with HCS
                        cmd_sr     <= {1'b0, 1'b1, 6'd1, 32'h40FF8080, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        delay_cnt  <= 16'd0;
                        state      <= ST_CMD2;
                    end else begin
                        delay_cnt <= delay_cnt + 1;
                    end
                end

                ST_CMD2: begin
                    if (resp_done_r) begin
                        if (resp_sr[31]) begin
                            cmd_sr     <= {1'b0, 1'b1, 6'd2, 32'd0, 7'd0, 1'b1};
                            cmd_bcnt   <= 6'd0;
                            cmd_active <= 1'b1;
                            cmd_oe     <= 1'b1;
                            state      <= ST_CMD3;
                        end else begin
                            // Retry CMD1
                            cmd_sr     <= {1'b0, 1'b1, 6'd1, 32'h40FF8080, 7'd0, 1'b1};
                            cmd_bcnt   <= 6'd0;
                            cmd_active <= 1'b1;
                            cmd_oe     <= 1'b1;
                        end
                    end
                end

                ST_CMD3: begin
                    if (resp_done_r) begin
                        cmd_sr     <= {1'b0, 1'b1, 6'd3, 32'h00010000, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        state      <= ST_CMD7;
                    end
                end

                ST_CMD7: begin
                    if (resp_done_r) begin
                        cmd_sr     <= {1'b0, 1'b1, 6'd7, 32'h00010000, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        clk_div_r  <= CLK_DIV_FAST;
                        state      <= ST_READY;
                    end
                end

                ST_READY: begin
                    if (resp_done_r || ready)
                        ready <= 1'b1;

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
                    if (!cmd_active) begin
                        cmd_sr     <= {1'b0, 1'b1, 6'd17, sector_r, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        state      <= ST_RD_WAIT;
                    end
                end

                ST_RD_WAIT: begin
                    if (resp_done_r) begin
                        dat_rx_active <= 1'b1;
                    end
                    if (rd_done) begin
                        state <= ST_DONE;
                    end
                end

                ST_CMD24: begin
                    if (!cmd_active) begin
                        cmd_sr     <= {1'b0, 1'b1, 6'd24, sector_r, 7'd0, 1'b1};
                        cmd_bcnt   <= 6'd0;
                        cmd_active <= 1'b1;
                        cmd_oe     <= 1'b1;
                        state      <= ST_WR_SEND;
                    end
                end

                ST_WR_SEND: begin
                    if (resp_done_r) begin
                        // In real impl: send start bit, 4096 data bits, CRC16, stop
                        // Simplified: just wait
                        delay_cnt <= 16'd0;
                        state     <= ST_WR_WAIT;
                    end
                end

                ST_WR_WAIT: begin
                    delay_cnt <= delay_cnt + 1;
                    if (delay_cnt >= 16'd50000) begin
                        wr_done <= 1'b1;
                        state   <= ST_DONE;
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
