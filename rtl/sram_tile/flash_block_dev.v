// flash_block_dev.v — NOR Flash streaming block device
//
// 16-bit block address in → stream 64-bit words out
//
// Architecture:
//   - N flash chips grouped into banks of 4 (4 × 16-bit = 64-bit bus)
//   - FPGA muxes banks onto shared 64-bit output
//   - For each block address, FPGA sequences:
//       1. For each word offset within the block:
//          a. Set flash address = {block_addr, word_offset}
//          b. For each bank: assert CE#, wait tAA, capture 64 bits
//          c. Push 64 bits to output with valid strobe
//       2. Assert done
//
// With 24 chips (6 banks of 4), 8MB per chip:
//   - Flash has 2^23 = 8M words per chip
//   - 16-bit block addr → 65536 blocks
//   - Words per block per chip = 8M / 65536 = 128
//   - Bytes per block = 128 words × 2 bytes × 24 chips = 6144 bytes = 6KB
//   - Stream rate: 64 bits every ~130ns (3 clk addr + 3 clk tAA per bank)
//   - Block read time: 6KB / 8 bytes × 130ns × 6 banks = ~63μs
//
// Output interface:
//   dout[63:0]  — 64-bit data (8 bytes)
//   dout_valid  — high for one clock when dout is valid
//   block_done  — pulses when all data for this block has been streamed
//   ready       — high when idle, accepts new block_addr + start
//
// Input interface:
//   block_addr[15:0] — which block to read
//   start            — pulse to begin streaming
//   rw_n             — 1=read (stream out), 0=write (stream in)
//   din[63:0]        — write data (when writing)
//   din_valid        — write data valid strobe

module flash_block_dev #(
    parameter N_CHIPS       = 24,        // total flash chips
    parameter CHIPS_PER_BANK = 4,        // chips per 64-bit bank
    parameter FLASH_AW      = 23,        // flash address pins (8M words)
    parameter FLASH_DW      = 16,        // flash data width per chip
    parameter BLOCK_ADDR_W  = 16,        // block address width
    parameter BUS_W         = CHIPS_PER_BANK * FLASH_DW,  // 64-bit output
    parameter N_BANKS       = N_CHIPS / CHIPS_PER_BANK,
    // Words per block per chip = 2^FLASH_AW / 2^BLOCK_ADDR_W
    parameter WORDS_PER_BLOCK = (1 << FLASH_AW) >> BLOCK_ADDR_W,  // 128
    // Total bytes per block = WORDS_PER_BLOCK × 2 × N_CHIPS
    parameter BLOCK_BYTES   = WORDS_PER_BLOCK * (FLASH_DW/8) * N_CHIPS
)(
    input  wire                    clk,
    input  wire                    rst_n,

    // --- Host interface ---
    input  wire [BLOCK_ADDR_W-1:0] block_addr,
    input  wire                    rw_n,        // 1=read, 0=write
    input  wire                    start,
    output reg                     ready,

    // --- Read output (streaming) ---
    output reg  [BUS_W-1:0]       dout,
    output reg                     dout_valid,
    output reg                     block_done,

    // --- Write input (streaming) ---
    input  wire [BUS_W-1:0]       din,
    input  wire                    din_valid,

    // --- Flash pins ---
    output reg  [FLASH_AW-1:0]   flash_a,
    inout  wire [FLASH_DW*CHIPS_PER_BANK-1:0] flash_dq,  // 64-bit shared data bus
    output reg  [N_CHIPS-1:0]    flash_ce_n,
    output reg                    flash_oe_n,
    output reg                    flash_we_n,

    // --- Status ---
    output wire [2:0]             dbg_bank,
    output wire [6:0]             dbg_word_offset
);

    // =====================================================================
    // Internal signals
    // =====================================================================

    localparam WORD_OFFSET_W = FLASH_AW - BLOCK_ADDR_W;  // 7 bits for 128 words

    // State machine
    localparam S_IDLE       = 3'd0;
    localparam S_SET_ADDR   = 3'd1;
    localparam S_WAIT_TAA   = 3'd2;
    localparam S_CAPTURE    = 3'd3;
    localparam S_NEXT_BANK  = 3'd4;
    localparam S_NEXT_WORD  = 3'd5;
    localparam S_DONE       = 3'd6;
    // Write states
    localparam S_WRITE_SETUP = 3'd7;

    reg [2:0]  state;
    reg [2:0]  bank_idx;                          // current bank (0 to N_BANKS-1)
    reg [WORD_OFFSET_W-1:0] word_offset;          // word within block
    reg [BLOCK_ADDR_W-1:0]  block_addr_r;
    reg        rw_r;
    reg [2:0]  wait_cnt;

    // Flash address = {block_addr, word_offset}
    wire [FLASH_AW-1:0] flash_addr_read =
        {block_addr_r, word_offset};

    // CE# decode: enable only the 4 chips in current bank
    // Bank 0 = chips 0-3, Bank 1 = chips 4-7, etc.
    reg [N_CHIPS-1:0] ce_decode;
    integer ci;
    always @(*) begin
        ce_decode = {N_CHIPS{1'b1}};  // all disabled
        for (ci = 0; ci < CHIPS_PER_BANK; ci = ci + 1)
            ce_decode[bank_idx * CHIPS_PER_BANK + ci] = 1'b0;  // enable this bank
    end

    // Tristate: flash drives during read, FPGA drives during write
    reg                bus_drive;
    reg [BUS_W-1:0]   bus_out;
    assign flash_dq = bus_drive ? bus_out : {BUS_W{1'bz}};

    // Debug
    assign dbg_bank = bank_idx;
    assign dbg_word_offset = word_offset[6:0];

    // =====================================================================
    // Main state machine
    // =====================================================================

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            ready        <= 1'b1;
            dout         <= {BUS_W{1'b0}};
            dout_valid   <= 1'b0;
            block_done   <= 1'b0;
            flash_a      <= {FLASH_AW{1'b0}};
            flash_ce_n   <= {N_CHIPS{1'b1}};
            flash_oe_n   <= 1'b1;
            flash_we_n   <= 1'b1;
            bus_drive    <= 1'b0;
            bank_idx     <= 3'd0;
            word_offset  <= {WORD_OFFSET_W{1'b0}};
            block_addr_r <= {BLOCK_ADDR_W{1'b0}};
            rw_r         <= 1'b1;
            wait_cnt     <= 3'd0;
            bus_out      <= {BUS_W{1'b0}};
        end else begin
            dout_valid <= 1'b0;
            block_done <= 1'b0;

            case (state)
                // ---------------------------------------------------------
                S_IDLE: begin
                    ready      <= 1'b1;
                    flash_ce_n <= {N_CHIPS{1'b1}};
                    flash_oe_n <= 1'b1;
                    flash_we_n <= 1'b1;
                    bus_drive  <= 1'b0;

                    if (start) begin
                        block_addr_r <= block_addr;
                        rw_r         <= rw_n;
                        ready        <= 1'b0;
                        bank_idx     <= 3'd0;
                        word_offset  <= {WORD_OFFSET_W{1'b0}};
                        state        <= S_SET_ADDR;
                    end
                end

                // ---------------------------------------------------------
                // READ SEQUENCE
                // ---------------------------------------------------------
                S_SET_ADDR: begin
                    flash_a    <= flash_addr_read;
                    flash_ce_n <= ce_decode;
                    flash_oe_n <= 1'b0;
                    flash_we_n <= 1'b1;
                    bus_drive  <= 1'b0;
                    wait_cnt   <= 3'd0;
                    state      <= S_WAIT_TAA;
                end

                S_WAIT_TAA: begin
                    // Wait 3 clocks @ 30MHz = 100ns > 90ns tAA
                    wait_cnt <= wait_cnt + 1;
                    if (wait_cnt >= 3'd2) begin
                        state <= S_CAPTURE;
                    end
                end

                S_CAPTURE: begin
                    dout       <= flash_dq;
                    dout_valid <= 1'b1;
                    flash_ce_n <= {N_CHIPS{1'b1}};
                    flash_oe_n <= 1'b1;
                    state      <= S_NEXT_BANK;
                end

                S_NEXT_BANK: begin
                    if (bank_idx == N_BANKS - 1) begin
                        // All banks done for this word offset
                        bank_idx <= 3'd0;
                        state    <= S_NEXT_WORD;
                    end else begin
                        bank_idx <= bank_idx + 1;
                        state    <= S_SET_ADDR;
                    end
                end

                S_NEXT_WORD: begin
                    if (word_offset == WORDS_PER_BLOCK - 1) begin
                        state <= S_DONE;
                    end else begin
                        word_offset <= word_offset + 1;
                        state       <= S_SET_ADDR;
                    end
                end

                // ---------------------------------------------------------
                S_DONE: begin
                    block_done <= 1'b1;
                    state      <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
