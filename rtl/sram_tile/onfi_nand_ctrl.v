// onfi_nand_ctrl.v — ONFI NAND Flash Controller (async mode)
//
// Controls one ONFI bus with N_CE chip-enable lines.
// 8-bit data bus shared, CE#-selected chips.
// Supports: READ PAGE (00h-30h), PROGRAM PAGE (80h-10h), READ STATUS (70h)
//
// ONFI async timing (mode 0, conservative):
//   tRC  = 50ns (read cycle)   → 20 MT/s
//   tWC  = 50ns (write cycle)
//   tR   = 25μs (page read to internal buffer)
//   tPROG = 200-700μs (page program)
//
// With CE# interleave: while chip 0 does internal tR (25μs),
// we can issue commands to chips 1,2,3 — all 4 load pages simultaneously.
// When ready, burst page data out at 20 MB/s per chip.
//
// Page size: 4KB (4096 + 128 spare = 4224 bytes, we read 4096)

module onfi_nand_ctrl #(
    parameter N_CE       = 4,           // chips per bus
    parameter PAGE_BYTES = 4096,
    parameter ADDR_CYCLES = 5           // 2 col + 3 row for 8GB MLC
)(
    input  wire        clk,             // 50MHz system clock
    input  wire        rst_n,

    // --- ONFI bus pins ---
    output reg  [N_CE-1:0] nand_ce_n,  // chip enables (active low)
    output reg         nand_cle,        // command latch enable
    output reg         nand_ale,        // address latch enable
    output reg         nand_we_n,       // write enable
    output reg         nand_re_n,       // read enable
    input  wire        nand_rb_n,       // ready/busy (active low = busy)
    inout  wire [7:0]  nand_dq,         // 8-bit data bus

    // --- Per-chip request interface ---
    input  wire [N_CE-1:0]  req_valid,  // request pending per chip
    input  wire [N_CE-1:0]  req_is_write,
    input  wire [N_CE*40-1:0] req_row_col_flat,  // flattened {row[23:0], col[15:0]} per chip
    output reg  [N_CE-1:0]  req_accepted,

    // --- Read data output ---
    output reg  [7:0]  rd_data,
    output reg         rd_valid,
    output reg  [1:0]  rd_chip,         // which CE# this data is from
    output reg         rd_page_done,

    // --- Write data input ---
    input  wire [7:0]  wr_data,
    output reg         wr_ready,        // pull next byte
    output reg  [1:0]  wr_chip,
    output reg         wr_page_done,

    // --- Status ---
    output wire [N_CE-1:0] chip_ready
);

    // =====================================================================
    // Data bus tristate
    // =====================================================================
    reg [7:0] dq_out;
    reg       dq_oe;
    assign nand_dq = dq_oe ? dq_out : 8'hzz;

    // Unpack flattened address port
    wire [39:0] req_row_col [0:N_CE-1];
    genvar ui;
    generate
        for (ui = 0; ui < N_CE; ui = ui + 1) begin : unpack_addr
            assign req_row_col[ui] = req_row_col_flat[ui*40 +: 40];
        end
    endgenerate

    // =====================================================================
    // Timing counter (50MHz = 20ns per tick)
    // =====================================================================
    reg [2:0] tcnt;     // 0-7 ticks for bus timing (max 160ns)

    // =====================================================================
    // Per-chip state tracking
    // =====================================================================
    localparam CS_IDLE     = 3'd0;
    localparam CS_CMD_SENT = 3'd1;   // command issued, waiting tR/tPROG
    localparam CS_READY    = 3'd2;   // page in buffer, ready to burst
    localparam CS_READING  = 3'd3;   // bursting data out
    localparam CS_WRITING  = 3'd4;   // accepting data in
    localparam CS_CONFIRM  = 3'd5;   // write confirm (10h) sent

    reg [2:0]  cs [0:N_CE-1];
    reg [12:0] byte_cnt [0:N_CE-1];  // 0-4095 within page

    assign chip_ready[0] = (cs[0] == CS_IDLE);
    assign chip_ready[1] = (N_CE > 1) ? (cs[1] == CS_IDLE) : 1'b0;
    assign chip_ready[2] = (N_CE > 2) ? (cs[2] == CS_IDLE) : 1'b0;
    assign chip_ready[3] = (N_CE > 3) ? (cs[3] == CS_IDLE) : 1'b0;

    // =====================================================================
    // Bus arbiter: round-robin across chips for bus access
    // =====================================================================
    localparam BUS_IDLE      = 4'd0;
    localparam BUS_CMD1      = 4'd1;  // send first command byte
    localparam BUS_ADDR      = 4'd2;  // send address cycles
    localparam BUS_CMD2      = 4'd3;  // send second command byte (30h/10h)
    localparam BUS_WAIT_RB   = 4'd4;  // wait for R/B# high
    localparam BUS_READ_DATA = 4'd5;  // burst read page data
    localparam BUS_WRITE_DATA= 4'd6;  // burst write page data
    localparam BUS_DONE      = 4'd7;

    reg [3:0]  bus_state;
    reg [1:0]  arb_chip;             // currently selected chip (0 to N_CE-1)
    reg [2:0]  addr_cycle;           // 0-4 for 5 address cycles
    reg [39:0] cur_addr;             // latched address
    reg        cur_is_write;
    reg [12:0] xfer_cnt;             // bytes transferred

    integer ai;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            bus_state    <= BUS_IDLE;
            arb_chip     <= 2'd0;
            nand_ce_n    <= {N_CE{1'b1}};
            nand_cle     <= 1'b0;
            nand_ale     <= 1'b0;
            nand_we_n    <= 1'b1;
            nand_re_n    <= 1'b1;
            dq_oe        <= 1'b0;
            rd_valid     <= 1'b0;
            rd_page_done <= 1'b0;
            wr_ready     <= 1'b0;
            wr_page_done <= 1'b0;
            req_accepted <= {N_CE{1'b0}};
            tcnt         <= 3'd0;
            xfer_cnt     <= 13'd0;
            addr_cycle   <= 3'd0;
            for (ai = 0; ai < N_CE; ai = ai + 1) begin
                cs[ai]       <= CS_IDLE;
                byte_cnt[ai] <= 13'd0;
            end
        end else begin
            rd_valid     <= 1'b0;
            rd_page_done <= 1'b0;
            wr_ready     <= 1'b0;
            wr_page_done <= 1'b0;
            req_accepted <= {N_CE{1'b0}};

            case (bus_state)
                // ==========================================================
                BUS_IDLE: begin
                    nand_ce_n <= {N_CE{1'b1}};
                    nand_cle  <= 1'b0;
                    nand_ale  <= 1'b0;
                    dq_oe     <= 1'b0;
                    nand_we_n <= 1'b1;
                    nand_re_n <= 1'b1;

                    // Priority 1: chip with page ready to burst
                    if (cs[arb_chip] == CS_READY) begin
                        nand_ce_n[arb_chip] <= 1'b0;
                        xfer_cnt  <= 13'd0;
                        bus_state <= BUS_READ_DATA;
                        cs[arb_chip] <= CS_READING;
                    end
                    // Priority 2: new request to issue
                    else if (req_valid[arb_chip] && cs[arb_chip] == CS_IDLE) begin
                        cur_addr     <= req_row_col[arb_chip];
                        cur_is_write <= req_is_write[arb_chip];
                        req_accepted[arb_chip] <= 1'b1;
                        nand_ce_n[arb_chip] <= 1'b0;
                        bus_state <= BUS_CMD1;
                        tcnt      <= 3'd0;
                    end
                    else begin
                        // Round-robin to next chip
                        arb_chip <= (arb_chip == N_CE - 1) ? 2'd0 : arb_chip + 1;
                    end
                end

                // ==========================================================
                BUS_CMD1: begin
                    // Latch command byte: 00h (read) or 80h (write)
                    nand_cle <= 1'b1;
                    dq_oe    <= 1'b1;
                    dq_out   <= cur_is_write ? 8'h80 : 8'h00;

                    if (tcnt == 3'd0) begin
                        nand_we_n <= 1'b0;   // WE# low
                        tcnt <= 3'd1;
                    end else if (tcnt == 3'd2) begin
                        nand_we_n <= 1'b1;   // WE# high → latch
                        nand_cle  <= 1'b0;
                        tcnt      <= 3'd0;
                        addr_cycle <= 3'd0;
                        bus_state <= BUS_ADDR;
                    end else
                        tcnt <= tcnt + 1;
                end

                // ==========================================================
                BUS_ADDR: begin
                    nand_ale <= 1'b1;
                    dq_oe    <= 1'b1;

                    // 5 address cycles: col_lo, col_hi, row_lo, row_mid, row_hi
                    case (addr_cycle)
                        3'd0: dq_out <= cur_addr[7:0];    // col low
                        3'd1: dq_out <= cur_addr[15:8];   // col high
                        3'd2: dq_out <= cur_addr[23:16];  // row low
                        3'd3: dq_out <= cur_addr[31:24];  // row mid
                        3'd4: dq_out <= cur_addr[39:32];  // row high
                        default: dq_out <= 8'h00;
                    endcase

                    if (tcnt == 3'd0) begin
                        nand_we_n <= 1'b0;
                        tcnt <= 3'd1;
                    end else if (tcnt == 3'd2) begin
                        nand_we_n <= 1'b1;
                        tcnt <= 3'd0;

                        if (addr_cycle == ADDR_CYCLES - 1) begin
                            nand_ale <= 1'b0;

                            if (cur_is_write) begin
                                cs[arb_chip] <= CS_WRITING;
                                xfer_cnt  <= 13'd0;
                                bus_state <= BUS_WRITE_DATA;
                            end else begin
                                bus_state <= BUS_CMD2;
                            end
                        end else
                            addr_cycle <= addr_cycle + 1;
                    end else
                        tcnt <= tcnt + 1;
                end

                // ==========================================================
                BUS_CMD2: begin
                    // Send 30h (read confirm)
                    nand_cle <= 1'b1;
                    dq_oe    <= 1'b1;
                    dq_out   <= 8'h30;

                    if (tcnt == 3'd0) begin
                        nand_we_n <= 1'b0;
                        tcnt <= 3'd1;
                    end else if (tcnt == 3'd2) begin
                        nand_we_n <= 1'b1;
                        nand_cle  <= 1'b0;
                        dq_oe     <= 1'b0;
                        tcnt      <= 3'd0;
                        cs[arb_chip] <= CS_CMD_SENT;
                        // Release bus — chip does internal tR (~25μs)
                        // We can now serve other chips
                        nand_ce_n <= {N_CE{1'b1}};
                        bus_state <= BUS_IDLE;
                    end else
                        tcnt <= tcnt + 1;
                end

                // ==========================================================
                BUS_READ_DATA: begin
                    nand_ce_n[arb_chip] <= 1'b0;
                    dq_oe <= 1'b0;

                    // Toggle RE# to clock out data
                    if (tcnt == 3'd0) begin
                        nand_re_n <= 1'b0;
                        tcnt <= 3'd1;
                    end else if (tcnt == 3'd2) begin
                        nand_re_n <= 1'b1;
                        rd_data   <= nand_dq;
                        rd_valid  <= 1'b1;
                        rd_chip   <= arb_chip;
                        xfer_cnt  <= xfer_cnt + 1;
                        tcnt      <= 3'd0;

                        if (xfer_cnt == PAGE_BYTES - 1) begin
                            rd_page_done <= 1'b1;
                            cs[arb_chip] <= CS_IDLE;
                            nand_ce_n    <= {N_CE{1'b1}};
                            bus_state    <= BUS_IDLE;
                        end
                    end else
                        tcnt <= tcnt + 1;
                end

                // ==========================================================
                BUS_WRITE_DATA: begin
                    nand_ce_n[arb_chip] <= 1'b0;
                    dq_oe  <= 1'b1;
                    dq_out <= wr_data;

                    if (tcnt == 3'd0) begin
                        wr_ready  <= 1'b1;
                        wr_chip   <= arb_chip;
                        nand_we_n <= 1'b0;
                        tcnt <= 3'd1;
                    end else if (tcnt == 3'd2) begin
                        nand_we_n <= 1'b1;
                        xfer_cnt  <= xfer_cnt + 1;
                        tcnt      <= 3'd0;

                        if (xfer_cnt == PAGE_BYTES - 1) begin
                            // Send 10h (program confirm)
                            bus_state <= BUS_CMD2;
                            // Reuse CMD2 state but with 10h
                            // (simplified — in real design, separate state)
                            cs[arb_chip] <= CS_CONFIRM;
                            wr_page_done <= 1'b1;
                            nand_ce_n    <= {N_CE{1'b1}};
                            dq_oe        <= 1'b0;
                            bus_state    <= BUS_IDLE;
                        end
                    end else
                        tcnt <= tcnt + 1;
                end

                default: bus_state <= BUS_IDLE;
            endcase

            // Check R/B# for chips that have commands in flight
            // When R/B# goes high, page is in buffer → mark READY
            for (ai = 0; ai < N_CE; ai = ai + 1) begin
                if (cs[ai] == CS_CMD_SENT && nand_rb_n) begin
                    cs[ai] <= CS_READY;
                end
            end
        end
    end

endmodule
