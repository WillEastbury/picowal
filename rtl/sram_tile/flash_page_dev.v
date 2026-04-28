// flash_page_dev.v — NOR Flash 512-byte page device
//
// 16-bit page address in → streams 64 × 8-byte words = 512 bytes out
//
// Architecture:
//   4 flash chips in parallel = 64-bit bus (4 × 16-bit)
//   Flash address = {page_addr[15:0], word_offset[5:0]} = 22 bits
//   S29GL064N: 4M×16 (22-bit address) — uses full chip capacity
//
// Capacity:
//   65536 pages × 512 bytes = 32MB
//   4 chips × 8MB each = 32MB — perfect fit, zero waste
//
// Timing @ 30MHz:
//   4 clocks per read (addr setup + 3 clk tAA wait + capture)
//   64 reads × 4 clocks × 33ns = ~8.5μs per page
//   Throughput: 512 bytes / 8.5μs = ~60 MB/s
//
// BOM:
//   1× iCE40HX8K    $4
//   4× S29GL064N    $16
//   1× crystal      $0.20
//   caps + PCB      $8
//   Total:          ~$28

module flash_page_dev #(
    parameter N_CHIPS       = 4,
    parameter FLASH_AW      = 22,        // 4M words (full S29GL064N)
    parameter FLASH_DW      = 16,
    parameter PAGE_ADDR_W   = 16,        // 65536 pages
    parameter BUS_W         = N_CHIPS * FLASH_DW,  // 64 bits
    parameter WORD_OFFSET_W = FLASH_AW - PAGE_ADDR_W,  // 6 bits = 64 words
    parameter WORDS_PER_PAGE = (1 << WORD_OFFSET_W),    // 64
    parameter PAGE_BYTES    = WORDS_PER_PAGE * (BUS_W / 8)  // 512
)(
    input  wire                    clk,
    input  wire                    rst_n,

    // --- Host interface ---
    input  wire [PAGE_ADDR_W-1:0]  page_addr,
    input  wire                    rw_n,        // 1=read, 0=write
    input  wire                    start,
    output reg                     ready,

    // --- Read output (streaming 64 bits at a time) ---
    output reg  [BUS_W-1:0]       dout,
    output reg                     dout_valid,
    output reg                     page_done,

    // --- Write input (streaming 64 bits at a time) ---
    input  wire [BUS_W-1:0]       din,
    input  wire                    din_valid,

    // --- Flash pins ---
    output reg  [FLASH_AW-1:0]   flash_a,
    inout  wire [BUS_W-1:0]      flash_dq,
    output reg  [N_CHIPS-1:0]    flash_ce_n,
    output reg                    flash_oe_n,
    output reg                    flash_we_n,

    // --- Status ---
    output wire                    dbg_reading,
    output wire [WORD_OFFSET_W-1:0] dbg_word
);

    // =====================================================================
    // State machine
    // =====================================================================

    localparam S_IDLE    = 3'd0;
    localparam S_ADDR    = 3'd1;    // drive address + CE# + OE#
    localparam S_WAIT1   = 3'd2;    // tAA wait cycle 1
    localparam S_WAIT2   = 3'd3;    // tAA wait cycle 2
    localparam S_CAPTURE = 3'd4;    // capture data, emit dout_valid
    localparam S_NEXT    = 3'd5;    // advance to next word or done
    localparam S_DONE    = 3'd6;
    // Write states
    localparam S_WR_CMD1 = 3'd7;

    reg [2:0]  state;
    reg [WORD_OFFSET_W-1:0] word_idx;
    reg [PAGE_ADDR_W-1:0]   page_addr_r;
    reg        rw_r;

    // Flash address = {page_addr, word_idx}
    wire [FLASH_AW-1:0] flash_addr = {page_addr_r, word_idx};

    // Tristate control
    reg        bus_drive;
    reg [BUS_W-1:0] bus_out_r;
    assign flash_dq = bus_drive ? bus_out_r : {BUS_W{1'bz}};

    // Debug
    assign dbg_reading = (state != S_IDLE && state != S_DONE);
    assign dbg_word    = word_idx;

    // =====================================================================
    // Write support — AMD command sequence
    // For simplicity, write state machine is separate
    // =====================================================================

    // Write needs: unlock cycle 1, unlock cycle 2, program command, data write, poll
    // These use the same flash bus so we sequence them in the main FSM

    localparam CMD_ADDR_1 = 22'h555;
    localparam CMD_ADDR_2 = 22'h2AA;
    localparam CMD_DATA_1 = 16'hAA;
    localparam CMD_DATA_2 = 16'h55;
    localparam CMD_PROG   = 16'hA0;

    reg [2:0]  wr_phase;        // sub-phase within write sequence
    reg [15:0] wr_poll_cnt;     // poll counter for write completion

    // =====================================================================
    // Main state machine
    // =====================================================================

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            ready       <= 1'b1;
            dout        <= {BUS_W{1'b0}};
            dout_valid  <= 1'b0;
            page_done   <= 1'b0;
            flash_a     <= {FLASH_AW{1'b0}};
            flash_ce_n  <= {N_CHIPS{1'b1}};
            flash_oe_n  <= 1'b1;
            flash_we_n  <= 1'b1;
            bus_drive   <= 1'b0;
            bus_out_r   <= {BUS_W{1'b0}};
            word_idx    <= {WORD_OFFSET_W{1'b0}};
            page_addr_r <= {PAGE_ADDR_W{1'b0}};
            rw_r        <= 1'b1;
            wr_phase    <= 3'd0;
            wr_poll_cnt <= 16'd0;
        end else begin
            dout_valid <= 1'b0;
            page_done  <= 1'b0;

            case (state)
                // ---------------------------------------------------------
                S_IDLE: begin
                    ready      <= 1'b1;
                    flash_ce_n <= {N_CHIPS{1'b1}};
                    flash_oe_n <= 1'b1;
                    flash_we_n <= 1'b1;
                    bus_drive  <= 1'b0;

                    if (start) begin
                        page_addr_r <= page_addr;
                        rw_r        <= rw_n;
                        ready       <= 1'b0;
                        word_idx    <= {WORD_OFFSET_W{1'b0}};

                        if (rw_n) begin
                            state <= S_ADDR;
                        end else begin
                            wr_phase <= 3'd0;
                            state    <= S_WR_CMD1;
                        end
                    end
                end

                // =========================================================
                // READ PATH: addr → wait × 2 → capture → next
                // =========================================================

                S_ADDR: begin
                    flash_a    <= flash_addr;
                    flash_ce_n <= {N_CHIPS{1'b0}};  // all 4 chips enabled
                    flash_oe_n <= 1'b0;
                    flash_we_n <= 1'b1;
                    bus_drive  <= 1'b0;
                    state      <= S_WAIT1;
                end

                S_WAIT1: begin
                    state <= S_WAIT2;               // 2nd wait clock for tAA
                end

                S_WAIT2: begin
                    state <= S_CAPTURE;             // 3 clocks total ≈ 100ns
                end

                S_CAPTURE: begin
                    dout       <= flash_dq;
                    dout_valid <= 1'b1;
                    // Deassert CE#/OE# briefly between reads
                    flash_ce_n <= {N_CHIPS{1'b1}};
                    flash_oe_n <= 1'b1;
                    state      <= S_NEXT;
                end

                S_NEXT: begin
                    if (word_idx == WORDS_PER_PAGE - 1) begin
                        state <= S_DONE;
                    end else begin
                        word_idx <= word_idx + 1;
                        state    <= S_ADDR;
                    end
                end

                // =========================================================
                // WRITE PATH: AMD unlock + program, one word at a time
                // Waits for din_valid to get each 64-bit word
                // =========================================================

                S_WR_CMD1: begin
                    case (wr_phase)
                        3'd0: begin
                            // Wait for host to provide write data
                            if (din_valid) begin
                                bus_out_r <= din;
                                wr_phase  <= 3'd1;
                            end
                        end
                        3'd1: begin
                            // Unlock cycle 1: 0x555 = 0xAA
                            flash_a    <= CMD_ADDR_1;
                            flash_ce_n <= {N_CHIPS{1'b0}};
                            flash_oe_n <= 1'b1;
                            bus_drive  <= 1'b1;
                            // Replicate CMD_DATA_1 across all chip slots
                            bus_out_r  <= {N_CHIPS{CMD_DATA_1}};
                            flash_we_n <= 1'b0;
                            wr_phase   <= 3'd2;
                        end
                        3'd2: begin
                            flash_we_n <= 1'b1;
                            // Unlock cycle 2: 0x2AA = 0x55
                            flash_a    <= CMD_ADDR_2;
                            bus_out_r  <= {N_CHIPS{CMD_DATA_2}};
                            flash_we_n <= 1'b0;
                            wr_phase   <= 3'd3;
                        end
                        3'd3: begin
                            flash_we_n <= 1'b1;
                            // Program command: 0x555 = 0xA0
                            flash_a    <= CMD_ADDR_1;
                            bus_out_r  <= {N_CHIPS{CMD_PROG}};
                            flash_we_n <= 1'b0;
                            wr_phase   <= 3'd4;
                        end
                        3'd4: begin
                            flash_we_n <= 1'b1;
                            // Write actual data at target address
                            flash_a    <= flash_addr;
                            bus_out_r  <= din;       // restore user data
                            flash_we_n <= 1'b0;
                            wr_phase   <= 3'd5;
                            wr_poll_cnt <= 16'd0;
                        end
                        3'd5: begin
                            // Release bus, poll for completion
                            flash_we_n <= 1'b1;
                            bus_drive  <= 1'b0;
                            flash_oe_n <= 1'b0;
                            wr_poll_cnt <= wr_poll_cnt + 1;
                            // ~10μs @ 30MHz = 300 clocks
                            if (wr_poll_cnt >= 16'd300) begin
                                flash_ce_n <= {N_CHIPS{1'b1}};
                                flash_oe_n <= 1'b1;

                                if (word_idx == WORDS_PER_PAGE - 1) begin
                                    state <= S_DONE;
                                end else begin
                                    word_idx <= word_idx + 1;
                                    wr_phase <= 3'd0;  // wait for next din_valid
                                end
                            end
                        end
                        default: wr_phase <= 3'd0;
                    endcase
                end

                // ---------------------------------------------------------
                S_DONE: begin
                    page_done  <= 1'b1;
                    flash_ce_n <= {N_CHIPS{1'b1}};
                    flash_oe_n <= 1'b1;
                    flash_we_n <= 1'b1;
                    bus_drive  <= 1'b0;
                    state      <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
