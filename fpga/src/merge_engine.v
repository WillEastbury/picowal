`timescale 1ns / 1ps
//============================================================================
// merge_engine.v — Field-Level Delta Merge Engine for iCE40 HX8K
//
// Merges a delta (partial field update) into an existing 2 KB card stored on
// SD.  Two inferred BRAM buffers hold the old and new card images.  A small
// register file holds incoming delta bytes (max 256 B).
//
// Card format (2048 bytes = 4 × 512 B SD blocks):
//   [0x000] magic   uint16 LE  (0xCA7D → byte[0]=0x7D, byte[1]=0xCA)
//   [0x002] version uint16 LE
//   [0x004] fields…
//     Each field: [ordinal_byte][length_byte][data…]
//       ordinal_byte: bits[4:0]=ordinal (0-31), bits[7:5]=flags
//       length_byte:  uint8 payload length (0-255)
//     Remaining bytes: zero-padded to 2048
//
// Algorithm:
//   1. Stream old card into OLD_BUF (or zero-fill if is_new_card).
//   2. Stream delta bytes into DELTA_BUF, parse field headers → bitmap.
//   3. Build NEW_BUF:
//      a. Copy magic from OLD_BUF; increment version.
//      b. Walk OLD_BUF fields: replace if bitmap[ord] set, else copy.
//      c. Append delta fields whose ordinals were not in the old card.
//      d. Zero-pad to 2048 bytes.
//   4. Stream NEW_BUF out as new_data.
//============================================================================

module merge_engine (
    // System
    input  wire        clk,
    input  wire        rst_n,

    // Command
    input  wire        merge_start,
    input  wire        is_new_card,    // 1 = no existing card on SD

    // Old card input (streamed from SD read, 2048 bytes)
    input  wire [7:0]  old_data,
    input  wire        old_valid,
    input  wire        old_done,

    // Delta input (streamed from HTTP body)
    input  wire [7:0]  delta_data,
    input  wire        delta_valid,
    input  wire        delta_done,

    // New card output (streamed to SD write, 2048 bytes)
    output reg  [7:0]  new_data,
    output reg         new_valid,
    output reg         new_done,

    // Status
    output reg         merge_busy,
    output reg         merge_done,
    output reg         merge_error
);

    // ── Parameters ──────────────────────────────────────────────────────
    localparam CARD_SIZE  = 2048;
    localparam ADDR_W     = 11;          // log2(2048)
    localparam DELTA_SIZE = 256;
    localparam DELTA_AW   = 8;           // log2(256)

    // Card magic (little-endian)
    localparam MAGIC_LO = 8'h7D;
    localparam MAGIC_HI = 8'hCA;

    // ── BRAM buffers (inferred — Yosys maps to SB_RAM256x16) ───────────
    reg [7:0] old_buf  [0:CARD_SIZE-1];
    reg [7:0] new_buf  [0:CARD_SIZE-1];
    reg [7:0] delta_buf[0:DELTA_SIZE-1];

    // ── Bitmap: one bit per field ordinal (0-31) ────────────────────────
    reg [31:0] delta_bitmap;             // ordinals present in delta
    reg [31:0] old_bitmap;               // ordinals present in old card

    // ── Pointers / counters ─────────────────────────────────────────────
    reg [ADDR_W-1:0] old_wr_ptr;         // write pointer into old_buf
    reg [DELTA_AW-1:0] delta_wr_ptr;     // write pointer into delta_buf
    reg [DELTA_AW-1:0] delta_len;        // total delta bytes received
    reg [ADDR_W-1:0] old_rd_ptr;         // read pointer for old_buf scan
    reg [ADDR_W-1:0] new_wr_ptr;         // write pointer into new_buf
    reg [ADDR_W-1:0] new_rd_ptr;         // read pointer for output stream
    reg [DELTA_AW-1:0] delta_rd_ptr;     // read pointer for delta scan

    // Field parse temporaries
    reg [4:0]  field_ord;
    reg [7:0]  field_flags_ord;          // raw ordinal byte
    reg [7:0]  field_len;
    reg [7:0]  copy_cnt;                 // bytes remaining in current copy
    reg [15:0] old_version;

    // ── FSM states ──────────────────────────────────────────────────────
    localparam [4:0]
        S_IDLE           = 5'd0,
        S_LOAD_OLD       = 5'd1,
        S_LOAD_DELTA     = 5'd2,
        S_PARSE_DELTA    = 5'd3,
        S_BUILD_HDR0     = 5'd4,   // write magic lo to new_buf
        S_BUILD_HDR1     = 5'd21,  // write magic hi
        S_BUILD_HDR2     = 5'd22,  // write version lo (incremented)
        S_BUILD_HDR3     = 5'd23,  // write version hi (carry)
        S_SCAN_OLD_HDR0  = 5'd5,   // read ordinal byte from old_buf
        S_SCAN_OLD_HDR1  = 5'd6,   // read length byte from old_buf
        S_WRITE_OLD_LEN  = 5'd24,  // write kept-field length to new_buf
        S_COPY_OLD_FIELD = 5'd7,   // copy old field to new_buf
        S_COPY_DELTA_FLD = 5'd8,   // copy delta field to new_buf (replacement)
        S_SKIP_OLD_FIELD = 5'd9,   // skip old field data (replaced by delta)
        S_FIND_DELTA_HDR = 5'd10,  // scan delta for replacement field header
        S_FIND_DELTA_LEN = 5'd11,  // write delta ord byte to new_buf
        S_WRITE_DLTA_LEN = 5'd25,  // write delta length byte to new_buf
        S_APPEND_NEW     = 5'd12,  // append delta fields not in old card
        S_APPEND_HDR0    = 5'd13,
        S_APPEND_HDR1    = 5'd14,
        S_APPEND_DATA    = 5'd15,
        S_ZERO_PAD       = 5'd16,
        S_OUTPUT         = 5'd17,
        S_DONE           = 5'd18,
        S_ERROR          = 5'd19,
        S_LOAD_ZERO      = 5'd20;

    reg [4:0] state;

    // Delta parse pointer for building bitmap
    reg [DELTA_AW-1:0] dp_ptr;

    // For append-new pass: which ordinal we're scanning for
    reg [5:0]  append_ord;               // 0-31 + overflow sentinel
    reg [DELTA_AW-1:0] append_dp_ptr;    // delta scan pointer for append
    reg [7:0]  append_len;
    reg [7:0]  append_cnt;

    // For find-delta-field: scan pointer
    reg [DELTA_AW-1:0] fd_ptr;
    reg [7:0]  fd_len;                   // length of found delta field
    reg [DELTA_AW-1:0] fd_data_ptr;      // start of found delta field data
    reg [7:0]  fd_cnt;                   // copy counter for delta field

    // ── Delta parallel-capture flag ─────────────────────────────────────
    // Delta bytes can arrive while old card is still loading.  We capture
    // them into delta_buf regardless of FSM state and track completion.
    reg        delta_received;           // delta_done seen during LOAD_OLD/ZERO

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            merge_busy   <= 1'b0;
            merge_done   <= 1'b0;
            merge_error  <= 1'b0;
            new_data     <= 8'd0;
            new_valid    <= 1'b0;
            new_done     <= 1'b0;
            old_wr_ptr   <= {ADDR_W{1'b0}};
            delta_wr_ptr <= {DELTA_AW{1'b0}};
            delta_len    <= {DELTA_AW{1'b0}};
            old_rd_ptr   <= {ADDR_W{1'b0}};
            new_wr_ptr   <= {ADDR_W{1'b0}};
            new_rd_ptr   <= {ADDR_W{1'b0}};
            delta_rd_ptr <= {DELTA_AW{1'b0}};
            delta_bitmap <= 32'd0;
            old_bitmap   <= 32'd0;
            old_version  <= 16'd0;
            field_ord    <= 5'd0;
            field_flags_ord <= 8'd0;
            field_len    <= 8'd0;
            copy_cnt     <= 8'd0;
            dp_ptr       <= {DELTA_AW{1'b0}};
            append_ord   <= 6'd0;
            append_dp_ptr<= {DELTA_AW{1'b0}};
            append_len   <= 8'd0;
            append_cnt   <= 8'd0;
            fd_ptr       <= {DELTA_AW{1'b0}};
            fd_len       <= 8'd0;
            fd_data_ptr  <= {DELTA_AW{1'b0}};
            fd_cnt       <= 8'd0;
            delta_received <= 1'b0;
        end else begin
            // Default: clear one-cycle pulses
            new_valid  <= 1'b0;
            new_done   <= 1'b0;
            merge_done <= 1'b0;

            case (state)
            // ────────────────────────────────────────────────────────────
            // IDLE — wait for merge_start
            // ────────────────────────────────────────────────────────────
            S_IDLE: begin
                merge_error <= 1'b0;
                if (merge_start) begin
                    merge_busy   <= 1'b1;
                    old_wr_ptr   <= {ADDR_W{1'b0}};
                    delta_wr_ptr <= {DELTA_AW{1'b0}};
                    delta_len    <= {DELTA_AW{1'b0}};
                    new_wr_ptr   <= {ADDR_W{1'b0}};
                    new_rd_ptr   <= {ADDR_W{1'b0}};
                    delta_bitmap <= 32'd0;
                    old_bitmap   <= 32'd0;
                    old_version  <= 16'd0;
                    delta_received <= 1'b0;
                    if (is_new_card)
                        state <= S_LOAD_ZERO;
                    else
                        state <= S_LOAD_OLD;
                end
            end

            // ────────────────────────────────────────────────────────────
            // LOAD_ZERO — fill old_buf with zeros for a brand-new card
            //   Also captures delta bytes in parallel if they arrive early.
            // ────────────────────────────────────────────────────────────
            S_LOAD_ZERO: begin
                old_buf[old_wr_ptr] <= 8'd0;
                // Capture delta data in parallel (writes to separate BRAM)
                if (delta_valid && delta_wr_ptr < DELTA_SIZE[DELTA_AW-1:0]) begin
                    delta_buf[delta_wr_ptr] <= delta_data;
                    delta_wr_ptr <= delta_wr_ptr + 1'b1;
                end
                if (delta_done)
                    delta_received <= 1'b1;

                if (old_wr_ptr == CARD_SIZE[ADDR_W-1:0] - 1) begin
                    old_wr_ptr <= {ADDR_W{1'b0}};
                    if (delta_received || delta_done) begin
                        delta_len <= delta_wr_ptr;
                        dp_ptr    <= {DELTA_AW{1'b0}};
                        state     <= S_PARSE_DELTA;
                    end else begin
                        state <= S_LOAD_DELTA;
                    end
                end else begin
                    old_wr_ptr <= old_wr_ptr + 1'b1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // LOAD_OLD — stream old card bytes into old_buf
            //   Also captures delta bytes in parallel if they arrive early.
            // ────────────────────────────────────────────────────────────
            S_LOAD_OLD: begin
                if (old_valid) begin
                    old_buf[old_wr_ptr] <= old_data;
                    old_wr_ptr <= old_wr_ptr + 1'b1;
                end
                // Capture delta data in parallel (writes to separate BRAM)
                if (delta_valid && delta_wr_ptr < DELTA_SIZE[DELTA_AW-1:0]) begin
                    delta_buf[delta_wr_ptr] <= delta_data;
                    delta_wr_ptr <= delta_wr_ptr + 1'b1;
                end
                if (delta_done)
                    delta_received <= 1'b1;

                if (old_done) begin
                    if (delta_received || delta_done) begin
                        delta_len <= delta_wr_ptr;
                        dp_ptr    <= {DELTA_AW{1'b0}};
                        state     <= S_PARSE_DELTA;
                    end else begin
                        state <= S_LOAD_DELTA;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // LOAD_DELTA — stream delta bytes into delta_buf
            // ────────────────────────────────────────────────────────────
            S_LOAD_DELTA: begin
                if (delta_valid) begin
                    if (delta_wr_ptr < DELTA_SIZE[DELTA_AW-1:0]) begin
                        delta_buf[delta_wr_ptr] <= delta_data;
                        delta_wr_ptr <= delta_wr_ptr + 1'b1;
                    end
                    // Silently drop overflow beyond 256 bytes
                end
                if (delta_done) begin
                    delta_len <= delta_wr_ptr;
                    dp_ptr    <= {DELTA_AW{1'b0}};
                    state     <= S_PARSE_DELTA;
                end
            end

            // ────────────────────────────────────────────────────────────
            // PARSE_DELTA — walk delta fields, build delta_bitmap
            //   Each field: [ordinal_byte][length_byte][data…]
            // ────────────────────────────────────────────────────────────
            S_PARSE_DELTA: begin
                if (dp_ptr < delta_len) begin
                    // Read ordinal byte (combinational from register file)
                    if (dp_ptr + 1'b1 < delta_len) begin
                        // We have at least 2 bytes (ord + len)
                        delta_bitmap[delta_buf[dp_ptr][4:0]] <= 1'b1;
                        // Advance past header + payload
                        dp_ptr <= dp_ptr + 8'd2 + delta_buf[dp_ptr + 1'b1];
                    end else begin
                        // Malformed: ordinal byte with no length → error
                        state <= S_ERROR;
                    end
                end else begin
                    // All delta fields parsed → build new card header
                    state <= S_BUILD_HDR0;
                end
            end

            // ────────────────────────────────────────────────────────────
            // BUILD_HDR0..HDR3 — write magic + incremented version
            //   One new_buf write per cycle for clean BRAM inference.
            // ────────────────────────────────────────────────────────────
            S_BUILD_HDR0: begin
                old_version <= {old_buf[3], old_buf[2]};
                new_buf[0]  <= MAGIC_LO;
                new_wr_ptr  <= 11'd1;
                state       <= S_BUILD_HDR1;
            end

            S_BUILD_HDR1: begin
                new_buf[1]  <= MAGIC_HI;
                new_wr_ptr  <= 11'd2;
                state       <= S_BUILD_HDR2;
            end

            S_BUILD_HDR2: begin
                new_buf[2]  <= old_version[7:0] + 8'd1;
                new_wr_ptr  <= 11'd3;
                state       <= S_BUILD_HDR3;
            end

            S_BUILD_HDR3: begin
                new_buf[3]  <= (old_version[7:0] == 8'hFF)
                               ? old_version[15:8] + 8'd1
                               : old_version[15:8];
                new_wr_ptr  <= 11'd4;
                old_rd_ptr  <= 11'd4;
                state       <= S_SCAN_OLD_HDR0;
            end

            // ────────────────────────────────────────────────────────────
            // SCAN_OLD_HDR0 — read ordinal byte of next old field
            // ────────────────────────────────────────────────────────────
            S_SCAN_OLD_HDR0: begin
                // Check for end of old fields (zero byte = padding)
                if (old_rd_ptr >= CARD_SIZE[ADDR_W-1:0] ||
                    old_buf[old_rd_ptr] == 8'd0) begin
                    // No more old fields → append new delta fields
                    append_ord    <= 6'd0;
                    append_dp_ptr <= {DELTA_AW{1'b0}};
                    state         <= S_APPEND_NEW;
                end else begin
                    field_flags_ord <= old_buf[old_rd_ptr];
                    field_ord       <= old_buf[old_rd_ptr][4:0];
                    old_rd_ptr      <= old_rd_ptr + 1'b1;
                    state           <= S_SCAN_OLD_HDR1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // SCAN_OLD_HDR1 — read length byte of old field
            // ────────────────────────────────────────────────────────────
            S_SCAN_OLD_HDR1: begin
                field_len  <= old_buf[old_rd_ptr];
                old_rd_ptr <= old_rd_ptr + 1'b1;

                // Mark this ordinal as present in old card
                old_bitmap[field_ord] <= 1'b1;

                if (delta_bitmap[field_ord]) begin
                    // This ordinal is in the delta → replace
                    copy_cnt <= old_buf[old_rd_ptr];
                    state    <= S_SKIP_OLD_FIELD;
                end else begin
                    // Keep old field: write ordinal byte (one write/cycle)
                    new_buf[new_wr_ptr] <= field_flags_ord;
                    new_wr_ptr          <= new_wr_ptr + 1'b1;
                    copy_cnt            <= old_buf[old_rd_ptr];
                    state               <= S_WRITE_OLD_LEN;
                end
            end

            // ────────────────────────────────────────────────────────────
            // WRITE_OLD_LEN — write kept-field length byte to new_buf
            // ────────────────────────────────────────────────────────────
            S_WRITE_OLD_LEN: begin
                new_buf[new_wr_ptr] <= field_len;
                new_wr_ptr          <= new_wr_ptr + 1'b1;
                state               <= S_COPY_OLD_FIELD;
            end

            // ────────────────────────────────────────────────────────────
            // COPY_OLD_FIELD — copy old field payload to new_buf
            // ────────────────────────────────────────────────────────────
            S_COPY_OLD_FIELD: begin
                if (copy_cnt == 8'd0) begin
                    state <= S_SCAN_OLD_HDR0;
                end else begin
                    new_buf[new_wr_ptr] <= old_buf[old_rd_ptr];
                    new_wr_ptr <= new_wr_ptr + 1'b1;
                    old_rd_ptr <= old_rd_ptr + 1'b1;
                    copy_cnt   <= copy_cnt - 8'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // SKIP_OLD_FIELD — advance old_rd_ptr past replaced field data
            // ────────────────────────────────────────────────────────────
            S_SKIP_OLD_FIELD: begin
                if (copy_cnt == 8'd0) begin
                    // Now find the replacement field in delta_buf
                    fd_ptr <= {DELTA_AW{1'b0}};
                    state  <= S_FIND_DELTA_HDR;
                end else begin
                    old_rd_ptr <= old_rd_ptr + 1'b1;
                    copy_cnt   <= copy_cnt - 8'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // FIND_DELTA_HDR — scan delta_buf for matching ordinal
            // ────────────────────────────────────────────────────────────
            S_FIND_DELTA_HDR: begin
                if (fd_ptr >= delta_len) begin
                    // Should not happen (bitmap said it's present) → error
                    state <= S_ERROR;
                end else begin
                    if (delta_buf[fd_ptr][4:0] == field_ord) begin
                        // Found it — read length
                        fd_ptr <= fd_ptr + 1'b1;
                        state  <= S_FIND_DELTA_LEN;
                    end else begin
                        // Skip this delta field: advance by 2 + length
                        if (fd_ptr + 1'b1 < delta_len)
                            fd_ptr <= fd_ptr + 8'd2 + delta_buf[fd_ptr + 1'b1];
                        else
                            state <= S_ERROR;
                    end
                end
            end

            // ────────────────────────────────────────────────────────────
            // FIND_DELTA_LEN — read length byte of delta replacement
            // ────────────────────────────────────────────────────────────
            S_FIND_DELTA_LEN: begin
                fd_len      <= delta_buf[fd_ptr];
                fd_data_ptr <= fd_ptr + 1'b1;
                fd_cnt      <= delta_buf[fd_ptr];

                // Write delta ordinal byte (one write per cycle for BRAM)
                new_buf[new_wr_ptr] <= delta_buf[fd_ptr - 1'b1];
                new_wr_ptr          <= new_wr_ptr + 1'b1;
                state               <= S_WRITE_DLTA_LEN;
            end

            // ────────────────────────────────────────────────────────────
            // WRITE_DLTA_LEN — write delta length byte to new_buf
            // ────────────────────────────────────────────────────────────
            S_WRITE_DLTA_LEN: begin
                new_buf[new_wr_ptr] <= fd_len;
                new_wr_ptr          <= new_wr_ptr + 1'b1;
                state               <= S_COPY_DELTA_FLD;
            end

            // ────────────────────────────────────────────────────────────
            // COPY_DELTA_FLD — copy delta field payload to new_buf
            // ────────────────────────────────────────────────────────────
            S_COPY_DELTA_FLD: begin
                if (fd_cnt == 8'd0) begin
                    state <= S_SCAN_OLD_HDR0;
                end else begin
                    new_buf[new_wr_ptr] <= delta_buf[fd_data_ptr];
                    new_wr_ptr  <= new_wr_ptr + 1'b1;
                    fd_data_ptr <= fd_data_ptr + 1'b1;
                    fd_cnt      <= fd_cnt - 8'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // APPEND_NEW — find delta fields not in old card, append them
            //   Walk ordinals 0..31; for each set in delta_bitmap but not
            //   in old_bitmap, scan delta_buf and copy.
            // ────────────────────────────────────────────────────────────
            S_APPEND_NEW: begin
                if (append_ord >= 6'd32) begin
                    // Done appending → zero-pad
                    state <= S_ZERO_PAD;
                end else if (delta_bitmap[append_ord[4:0]] &&
                             !old_bitmap[append_ord[4:0]]) begin
                    // Need to append this ordinal — find it in delta_buf
                    append_dp_ptr <= {DELTA_AW{1'b0}};
                    state         <= S_APPEND_HDR0;
                end else begin
                    append_ord <= append_ord + 6'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // APPEND_HDR0 — scan delta_buf for the append ordinal
            // ────────────────────────────────────────────────────────────
            S_APPEND_HDR0: begin
                if (append_dp_ptr >= delta_len) begin
                    state <= S_ERROR;
                end else if (delta_buf[append_dp_ptr][4:0] == append_ord[4:0]) begin
                    // Found — write header to new_buf
                    new_buf[new_wr_ptr] <= delta_buf[append_dp_ptr];
                    new_wr_ptr          <= new_wr_ptr + 1'b1;
                    append_dp_ptr       <= append_dp_ptr + 1'b1;
                    state               <= S_APPEND_HDR1;
                end else begin
                    // Skip this delta field
                    if (append_dp_ptr + 1'b1 < delta_len)
                        append_dp_ptr <= append_dp_ptr + 8'd2
                                         + delta_buf[append_dp_ptr + 1'b1];
                    else
                        state <= S_ERROR;
                end
            end

            // ────────────────────────────────────────────────────────────
            // APPEND_HDR1 — read length byte, write to new_buf
            // ────────────────────────────────────────────────────────────
            S_APPEND_HDR1: begin
                append_len <= delta_buf[append_dp_ptr];
                append_cnt <= delta_buf[append_dp_ptr];
                new_buf[new_wr_ptr] <= delta_buf[append_dp_ptr];
                new_wr_ptr    <= new_wr_ptr + 1'b1;
                append_dp_ptr <= append_dp_ptr + 1'b1;
                state         <= S_APPEND_DATA;
            end

            // ────────────────────────────────────────────────────────────
            // APPEND_DATA — copy delta field payload to new_buf
            // ────────────────────────────────────────────────────────────
            S_APPEND_DATA: begin
                if (append_cnt == 8'd0) begin
                    append_ord <= append_ord + 6'd1;
                    state      <= S_APPEND_NEW;
                end else begin
                    new_buf[new_wr_ptr] <= delta_buf[append_dp_ptr];
                    new_wr_ptr    <= new_wr_ptr + 1'b1;
                    append_dp_ptr <= append_dp_ptr + 1'b1;
                    append_cnt    <= append_cnt - 8'd1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // ZERO_PAD — fill remainder of new_buf with zeros
            // ────────────────────────────────────────────────────────────
            S_ZERO_PAD: begin
                if (new_wr_ptr >= CARD_SIZE[ADDR_W-1:0]) begin
                    new_rd_ptr <= {ADDR_W{1'b0}};
                    state      <= S_OUTPUT;
                end else begin
                    new_buf[new_wr_ptr] <= 8'd0;
                    new_wr_ptr <= new_wr_ptr + 1'b1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // OUTPUT — stream new_buf to new_data output
            // ────────────────────────────────────────────────────────────
            S_OUTPUT: begin
                new_data  <= new_buf[new_rd_ptr];
                new_valid <= 1'b1;
                if (new_rd_ptr == CARD_SIZE[ADDR_W-1:0] - 1) begin
                    new_done <= 1'b1;
                    state    <= S_DONE;
                end else begin
                    new_rd_ptr <= new_rd_ptr + 1'b1;
                end
            end

            // ────────────────────────────────────────────────────────────
            // DONE — signal completion
            // ────────────────────────────────────────────────────────────
            S_DONE: begin
                merge_done <= 1'b1;
                merge_busy <= 1'b0;
                state      <= S_IDLE;
            end

            // ────────────────────────────────────────────────────────────
            // ERROR
            // ────────────────────────────────────────────────────────────
            S_ERROR: begin
                merge_error <= 1'b1;
                merge_busy  <= 1'b0;
                state       <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule
