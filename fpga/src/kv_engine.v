`timescale 1ns / 1ps
//============================================================================
// kv_engine.v — KV Operation Dispatcher for iCE40 HX8K
//
// Dispatches HTTP-parsed commands to SD card read/write operations via the
// merge engine.  Supports READ, WRITE, DELETE, LIST, and MGET operations.
//
// Key packing:  key = (pack[9:0] << 22) | card[21:0]
// SD address :  block_addr = key * 4  (each card = 4 × 512 B blocks)
//
// Field redaction: when pack==1 (users), ordinals 1 & 2 (pswdhash, salt)
//                  are stripped from the response stream.
//============================================================================

module kv_engine (
    // System
    input  wire        clk,
    input  wire        rst_n,

    // ── From HTTP parser ────────────────────────────────────────────────
    input  wire        cmd_valid,
    input  wire [2:0]  cmd_op,          // 0=READ 1=WRITE 2=DELETE 3=LIST 4=MGET
    input  wire [9:0]  cmd_pack,
    input  wire [21:0] cmd_card,

    // PUT body (for WRITE)
    input  wire [7:0]  body_data,
    input  wire        body_valid,
    input  wire        body_done,

    // LIST parameters
    input  wire [21:0] list_start,
    input  wire [15:0] list_limit,

    // MGET body (POST _mget: list of u32 card ordinals)
    input  wire [7:0]  mget_data,
    input  wire        mget_valid,
    input  wire        mget_done,

    // ── To SD controller ────────────────────────────────────────────────
    output reg         sd_cmd_start,
    output reg  [1:0]  sd_cmd_op,       // 0=read, 1=write
    output reg  [31:0] sd_block_addr,

    output reg  [7:0]  sd_write_data,
    output reg         sd_write_valid,

    input  wire [7:0]  sd_read_data,
    input  wire        sd_read_valid,
    input  wire        sd_cmd_done,

    // ── To response builder ─────────────────────────────────────────────
    output reg  [7:0]  resp_data,
    output reg         resp_valid,
    output reg         resp_done,
    output reg  [1:0]  resp_status,     // 00=200 01=204 10=404 11=500

    // ── To/from merge engine ────────────────────────────────────────────
    output reg         merge_start,
    output reg         merge_is_new_card,

    // Old card → merge engine
    output reg  [7:0]  merge_old_data,
    output reg         merge_old_valid,
    output reg         merge_old_done,

    // Delta → merge engine
    output reg  [7:0]  merge_delta_data,
    output reg         merge_delta_valid,
    output reg         merge_delta_done,

    // New card ← merge engine
    input  wire [7:0]  merge_new_data,
    input  wire        merge_new_valid,
    input  wire        merge_new_done,

    // Status ← merge engine
    input  wire        merge_busy,
    input  wire        merge_done,
    input  wire        merge_error
);

    // ── Op-code aliases ─────────────────────────────────────────────────
    localparam [2:0]
        OP_READ   = 3'd0,
        OP_WRITE  = 3'd1,
        OP_DELETE  = 3'd2,
        OP_LIST   = 3'd3,
        OP_MGET   = 3'd4;

    // SD op aliases
    localparam [1:0]
        SD_READ  = 2'd0,
        SD_WRITE = 2'd1;

    // Response status codes
    localparam [1:0]
        RESP_200 = 2'b00,
        RESP_204 = 2'b01,
        RESP_404 = 2'b10,
        RESP_500 = 2'b11;

    // Card parameters
    localparam CARD_SIZE   = 2048;
    localparam BLOCKS_PER  = 4;
    localparam MAGIC_LO    = 8'h7D;
    localparam MAGIC_HI    = 8'hCA;

    // Sentinel for LIST/MGET end
    localparam [31:0] END_SENTINEL = 32'hFFFFFFFF;

    // ── FSM states ──────────────────────────────────────────────────────
    localparam [5:0]
        S_IDLE             = 6'd0,
        // READ
        S_RD_SD_START      = 6'd1,
        S_RD_SD_WAIT       = 6'd2,
        S_RD_CHECK_MAGIC0  = 6'd3,
        S_RD_STREAM        = 6'd5,
        S_RD_DONE          = 6'd6,
        // WRITE
        S_WR_SD_READ       = 6'd7,
        S_WR_SD_WAIT       = 6'd8,
        S_WR_CHECK_MAGIC   = 6'd9,
        S_WR_REREAD        = 6'd10,   // re-read SD for merge old input
        S_WR_LOAD_DELTA    = 6'd11,
        S_WR_MERGE_WAIT    = 6'd12,
        S_WR_SD_WRITE      = 6'd13,
        S_WR_SD_WR_WAIT    = 6'd14,
        S_WR_STREAM_OLD    = 6'd47,   // stream re-read data to merge engine
        // DELETE
        S_DEL_SD_WRITE     = 6'd16,
        S_DEL_SD_WAIT      = 6'd17,
        S_DEL_DONE         = 6'd18,
        // LIST
        S_LST_NEXT         = 6'd19,
        S_LST_SD_START     = 6'd20,
        S_LST_SD_WAIT      = 6'd21,
        S_LST_CHECK_MAGIC0 = 6'd22,
        S_LST_CHECK_MAGIC1 = 6'd23,
        S_LST_SD_FULL      = 6'd24,
        S_LST_SD_FULL_WAIT = 6'd25,
        S_LST_STREAM_HDR   = 6'd26,
        S_LST_STREAM       = 6'd27,
        S_LST_SENTINEL     = 6'd28,
        S_LST_DONE         = 6'd29,
        // MGET
        S_MG_LOAD_BODY     = 6'd30,
        S_MG_NEXT          = 6'd31,
        S_MG_SD_START      = 6'd32,
        S_MG_SD_WAIT       = 6'd33,
        S_MG_CHECK_MAGIC0  = 6'd34,
        S_MG_CHECK_MAGIC1  = 6'd35,
        S_MG_STREAM_HDR    = 6'd36,
        S_MG_STREAM        = 6'd37,
        S_MG_STREAM_WAIT   = 6'd46,  // receive re-read data, stream to resp
        S_MG_SENTINEL      = 6'd38,
        S_MG_DONE          = 6'd39,
        // Shared
        S_RESP_DONE        = 6'd40,
        // Redaction sub-states (for READ)
        S_RD_FIELD_HDR0    = 6'd41,
        S_RD_FIELD_HDR1    = 6'd42,
        S_RD_FIELD_DATA    = 6'd43,
        S_RD_SKIP_FIELD    = 6'd44,
        S_RD_HDR_BYTES     = 6'd45;

    reg [5:0] state;

    // ── Latched command ─────────────────────────────────────────────────
    reg [2:0]  op_r;
    reg [9:0]  pack_r;
    reg [21:0] card_r;
    reg [21:0] list_start_r;
    reg [15:0] list_limit_r;

    // ── Address computation ─────────────────────────────────────────────
    // key = (pack << 22) | card
    // block_addr = key << 2  (= key * 4)
    // Note: 10-bit pack + 22-bit card + 2-bit shift = 34 bits, but SD block
    // addressing is 32 bits.  Effective limit: pack[7:0] (256 packs).
    wire [31:0] key_w   = {pack_r, card_r};
    wire [31:0] baddr_w = {key_w[29:0], 2'b00};

    // ── SD read buffer / counters ───────────────────────────────────────
    reg [10:0] sd_byte_cnt;              // counts bytes during SD read/write
    reg [7:0]  magic_byte0;             // captured first byte of card
    reg        is_existing;             // set if magic validated
    reg [10:0] payload_len;             // bytes of actual payload in card

    // ── LIST / MGET state ───────────────────────────────────────────────
    reg [21:0] scan_card;               // current card ordinal for LIST
    reg [15:0] scan_limit;              // remaining limit
    reg [21:0] scan_end;                // end of scan window

    localparam SCAN_WINDOW = 22'd256;   // how many card slots to probe

    // ── MGET body buffer ────────────────────────────────────────────────
    // Stores list of u32 card ordinals — max 64 entries (256 bytes)
    reg [7:0]  mget_buf [0:255];
    reg [7:0]  mget_len;                // total bytes received
    reg [7:0]  mget_ptr;                // current read pointer
    reg [7:0]  mget_wr_ptr;
    reg [21:0] mget_card_ord;           // parsed card ordinal

    // ── Redaction state ─────────────────────────────────────────────────
    reg        redact_active;           // 1 when pack==1
    reg [4:0]  fld_ord;                 // current field ordinal
    reg [7:0]  fld_len;                 // current field payload length
    reg [7:0]  fld_cnt;                 // field payload bytes remaining
    reg [10:0] stream_pos;              // byte position in card stream
    reg [7:0]  hdr_byte0;              // saved ordinal byte for 2-byte hdr emit

    // ── WRITE body-done latch ───────────────────────────────────────────
    // body_done may be a single-cycle pulse; latch it so the WRITE FSM
    // can check it reliably even if sd_cmd_done arrives later.
    reg        wr_body_done;

    // ── Sentinel emit counter ───────────────────────────────────────────
    reg [1:0]  sentinel_cnt;

    // ── LIST header emit ────────────────────────────────────────────────
    reg [2:0]  hdr_cnt;                 // counts header bytes emitted
    reg [10:0] lst_payload_len;         // payload length for current list entry

    // ── SD write stream counter ─────────────────────────────────────────
    reg [10:0] sd_wr_cnt;

    // ── Helper: compute block address for arbitrary card ─────────────────
    reg [31:0] scan_baddr;
    always @(*) begin
        scan_baddr = {pack_r, scan_card, 2'b00};
    end

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= S_IDLE;
            sd_cmd_start    <= 1'b0;
            sd_cmd_op       <= SD_READ;
            sd_block_addr   <= 32'd0;
            sd_write_data   <= 8'd0;
            sd_write_valid  <= 1'b0;
            resp_data       <= 8'd0;
            resp_valid      <= 1'b0;
            resp_done       <= 1'b0;
            resp_status     <= RESP_200;
            merge_start     <= 1'b0;
            merge_is_new_card <= 1'b0;
            merge_old_data  <= 8'd0;
            merge_old_valid <= 1'b0;
            merge_old_done  <= 1'b0;
            merge_delta_data  <= 8'd0;
            merge_delta_valid <= 1'b0;
            merge_delta_done  <= 1'b0;
            op_r            <= 3'd0;
            pack_r          <= 10'd0;
            card_r          <= 22'd0;
            list_start_r    <= 22'd0;
            list_limit_r    <= 16'd0;
            sd_byte_cnt     <= 11'd0;
            magic_byte0     <= 8'd0;
            is_existing     <= 1'b0;
            payload_len     <= 11'd0;
            scan_card       <= 22'd0;
            scan_limit      <= 16'd0;
            scan_end        <= 22'd0;
            mget_len        <= 8'd0;
            mget_ptr        <= 8'd0;
            mget_wr_ptr     <= 8'd0;
            mget_card_ord   <= 22'd0;
            redact_active   <= 1'b0;
            fld_ord         <= 5'd0;
            fld_len         <= 8'd0;
            fld_cnt         <= 8'd0;
            stream_pos      <= 11'd0;
            hdr_byte0       <= 8'd0;
            sentinel_cnt    <= 2'd0;
            hdr_cnt         <= 3'd0;
            lst_payload_len <= 11'd0;
            sd_wr_cnt       <= 11'd0;
            wr_body_done    <= 1'b0;
        end else begin
            // Default: clear one-cycle pulses
            sd_cmd_start    <= 1'b0;
            sd_write_valid  <= 1'b0;
            resp_valid      <= 1'b0;
            resp_done       <= 1'b0;
            merge_start     <= 1'b0;
            merge_old_valid <= 1'b0;
            merge_old_done  <= 1'b0;
            merge_delta_valid <= 1'b0;
            merge_delta_done  <= 1'b0;

            case (state)

            // ============================================================
            // IDLE — latch command and dispatch
            // ============================================================
            S_IDLE: begin
                if (cmd_valid) begin
                    op_r         <= cmd_op;
                    pack_r       <= cmd_pack;
                    card_r       <= cmd_card;
                    list_start_r <= list_start;
                    list_limit_r <= list_limit;
                    redact_active <= (cmd_pack == 10'd1);

                    case (cmd_op)
                        OP_READ:   state <= S_RD_SD_START;
                        OP_WRITE:  state <= S_WR_SD_READ;
                        OP_DELETE: state <= S_DEL_SD_WRITE;
                        OP_LIST: begin
                            scan_card  <= list_start;
                            scan_limit <= list_limit;
                            scan_end   <= list_start + SCAN_WINDOW;
                            state      <= S_LST_NEXT;
                        end
                        OP_MGET: begin
                            mget_wr_ptr <= 8'd0;
                            state       <= S_MG_LOAD_BODY;
                        end
                        default: begin
                            resp_status <= RESP_500;
                            state       <= S_RESP_DONE;
                        end
                    endcase
                end
            end

            // ============================================================
            //  KV_READ — read card from SD, stream with redaction
            // ============================================================

            // Start 4-block SD read
            S_RD_SD_START: begin
                sd_cmd_op    <= SD_READ;
                sd_block_addr <= baddr_w;
                sd_cmd_start <= 1'b1;
                sd_byte_cnt  <= 11'd0;
                state        <= S_RD_SD_WAIT;
            end

            // Wait for first byte (magic check)
            S_RD_SD_WAIT: begin
                if (sd_read_valid) begin
                    magic_byte0 <= sd_read_data;
                    sd_byte_cnt <= 11'd1;
                    state       <= S_RD_CHECK_MAGIC0;
                end
                if (sd_cmd_done && !sd_read_valid) begin
                    // SD returned nothing → error
                    resp_status <= RESP_500;
                    state       <= S_RESP_DONE;
                end
            end

            // Check magic byte 0
            S_RD_CHECK_MAGIC0: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    if (magic_byte0 == MAGIC_LO && sd_read_data == MAGIC_HI) begin
                        // Valid magic — set up streaming
                        resp_status  <= RESP_200;
                        is_existing  <= 1'b1;
                        stream_pos   <= 11'd0;
                        state        <= S_RD_HDR_BYTES;
                    end else begin
                        // No valid magic → 404
                        resp_status <= RESP_404;
                        state       <= S_RD_DONE;
                    end
                end
            end

            // Stream the 4-byte header (magic + version) directly
            S_RD_HDR_BYTES: begin
                // Emit magic byte 0
                resp_data  <= magic_byte0;
                resp_valid <= 1'b1;
                stream_pos <= 11'd2;     // next SD byte is byte[2]
                state      <= S_RD_STREAM;
                // Note: magic byte 1 and version bytes will stream through
                // S_RD_STREAM as raw bytes until stream_pos reaches 4,
                // then field parsing begins.
            end

            // Stream card bytes with redaction
            S_RD_STREAM: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;

                    if (stream_pos < 11'd4) begin
                        // Still in header (bytes 1-3) — emit directly
                        resp_data  <= sd_read_data;
                        resp_valid <= 1'b1;
                        stream_pos <= stream_pos + 1'b1;
                    end else if (sd_read_data == 8'd0 && stream_pos >= 11'd4) begin
                        // Zero byte = end of fields / padding — emit rest as-is
                        resp_data  <= sd_read_data;
                        resp_valid <= 1'b1;
                        stream_pos <= stream_pos + 1'b1;
                    end else if (stream_pos >= 11'd4) begin
                        // At a field boundary — read ordinal byte
                        hdr_byte0  <= sd_read_data;
                        fld_ord    <= sd_read_data[4:0];
                        stream_pos <= stream_pos + 1'b1;
                        state      <= S_RD_FIELD_HDR1;
                    end
                end
                if (sd_cmd_done) begin
                    state <= S_RD_DONE;
                end
            end

            // Read field length byte
            S_RD_FIELD_HDR1: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    fld_len     <= sd_read_data;
                    fld_cnt     <= sd_read_data;
                    stream_pos  <= stream_pos + 1'b1;

                    // Decide: redact or emit
                    if (redact_active &&
                        (fld_ord == 5'd1 || fld_ord == 5'd2)) begin
                        // Skip this field (don't emit header or data)
                        state <= S_RD_SKIP_FIELD;
                    end else begin
                        // Emit header bytes
                        resp_data  <= hdr_byte0;
                        resp_valid <= 1'b1;
                        state      <= S_RD_FIELD_HDR0;
                    end
                end
            end

            // Emit the length byte (second header byte), then data
            S_RD_FIELD_HDR0: begin
                resp_data  <= fld_len;
                resp_valid <= 1'b1;
                if (fld_cnt == 8'd0)
                    state <= S_RD_STREAM;  // zero-length field
                else
                    state <= S_RD_FIELD_DATA;
            end

            // Emit field data bytes
            S_RD_FIELD_DATA: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    stream_pos  <= stream_pos + 1'b1;
                    resp_data   <= sd_read_data;
                    resp_valid  <= 1'b1;
                    fld_cnt     <= fld_cnt - 8'd1;
                    if (fld_cnt == 8'd1)
                        state <= S_RD_STREAM;  // back to field boundary
                end
                if (sd_cmd_done) begin
                    state <= S_RD_DONE;
                end
            end

            // Skip redacted field data
            S_RD_SKIP_FIELD: begin
                if (fld_cnt == 8'd0) begin
                    state <= S_RD_STREAM;
                end else if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    stream_pos  <= stream_pos + 1'b1;
                    fld_cnt     <= fld_cnt - 8'd1;
                end
                if (sd_cmd_done) begin
                    state <= S_RD_DONE;
                end
            end

            // READ done — wait for SD to finish, then signal resp_done
            S_RD_DONE: begin
                if (sd_cmd_done || sd_byte_cnt >= CARD_SIZE[10:0]) begin
                    resp_done <= 1'b1;
                    state     <= S_IDLE;
                end
                // Drain any remaining SD bytes
                if (sd_read_valid)
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
            end

            // ============================================================
            //  KV_WRITE — read-modify-write via merge engine
            //
            //  Flow: SD read (magic check) → merge_start → re-read SD
            //        (stream old card to merge) → stream body delta →
            //        merge → SD write.  The merge engine captures delta
            //        bytes in parallel, so body can arrive during re-read.
            // ============================================================

            // Start SD read for magic check only
            S_WR_SD_READ: begin
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= baddr_w;
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                is_existing   <= 1'b0;
                state         <= S_WR_SD_WAIT;
            end

            // Receive SD bytes: check magic on first two, drain the rest.
            // Do NOT stream to merge engine yet (merge hasn't started).
            S_WR_SD_WAIT: begin
                if (sd_read_valid) begin
                    if (sd_byte_cnt == 11'd0)
                        magic_byte0 <= sd_read_data;
                    if (sd_byte_cnt == 11'd1) begin
                        if (magic_byte0 == MAGIC_LO && sd_read_data == MAGIC_HI)
                            is_existing <= 1'b1;
                    end
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                end
                if (sd_cmd_done) begin
                    state <= S_WR_CHECK_MAGIC;
                end
            end

            // Pulse merge_start, then branch based on card existence
            S_WR_CHECK_MAGIC: begin
                merge_is_new_card <= ~is_existing;
                merge_start       <= 1'b1;
                wr_body_done      <= 1'b0;  // clear latch for new operation
                if (is_existing)
                    state <= S_WR_REREAD;     // re-read card for merge
                else
                    state <= S_WR_LOAD_DELTA; // new card; merge zero-fills
            end

            // Start second SD read to stream old card to merge engine
            S_WR_REREAD: begin
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= baddr_w;
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_WR_STREAM_OLD;
            end

            // Stream re-read bytes to merge engine old-card input.
            // Body (delta) may arrive in parallel — merge engine captures it.
            S_WR_STREAM_OLD: begin
                if (sd_read_valid) begin
                    merge_old_data  <= sd_read_data;
                    merge_old_valid <= 1'b1;
                    sd_byte_cnt     <= sd_byte_cnt + 1'b1;
                end
                // Forward body to merge delta port in parallel
                if (body_valid) begin
                    merge_delta_data  <= body_data;
                    merge_delta_valid <= 1'b1;
                end
                if (body_done) begin
                    merge_delta_done <= 1'b1;
                    wr_body_done     <= 1'b1;
                end
                if (sd_cmd_done) begin
                    merge_old_done <= 1'b1;
                    if (wr_body_done || body_done)
                        state <= S_WR_MERGE_WAIT;
                    else
                        state <= S_WR_LOAD_DELTA;
                end
            end

            // Feed body (delta) to merge engine (if not already sent)
            S_WR_LOAD_DELTA: begin
                if (body_valid) begin
                    merge_delta_data  <= body_data;
                    merge_delta_valid <= 1'b1;
                end
                if (body_done) begin
                    merge_delta_done <= 1'b1;
                    state            <= S_WR_MERGE_WAIT;
                end
            end

            // Wait for merge engine to finish
            S_WR_MERGE_WAIT: begin
                if (merge_done) begin
                    if (merge_error) begin
                        resp_status <= RESP_500;
                        state       <= S_RESP_DONE;
                    end else begin
                        // Start SD write of merged card
                        sd_cmd_op     <= SD_WRITE;
                        sd_block_addr <= baddr_w;
                        sd_cmd_start  <= 1'b1;
                        sd_wr_cnt     <= 11'd0;
                        state         <= S_WR_SD_WRITE;
                    end
                end
            end

            // Stream merged card (from merge engine) to SD write
            S_WR_SD_WRITE: begin
                if (merge_new_valid) begin
                    sd_write_data  <= merge_new_data;
                    sd_write_valid <= 1'b1;
                    sd_wr_cnt      <= sd_wr_cnt + 1'b1;
                end
                if (merge_new_done) begin
                    state <= S_WR_SD_WR_WAIT;
                end
            end

            // Wait for SD write to complete
            S_WR_SD_WR_WAIT: begin
                if (sd_cmd_done) begin
                    resp_status <= RESP_200;
                    state       <= S_RESP_DONE;
                end
            end

            // ============================================================
            //  KV_DELETE — zero the magic bytes
            // ============================================================

            S_DEL_SD_WRITE: begin
                sd_cmd_op     <= SD_WRITE;
                sd_block_addr <= baddr_w;
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_DEL_SD_WAIT;
            end

            // Write 2 zero bytes then pad remainder (full 4-block write
            // with zeros — simple and ensures the block is cleared)
            S_DEL_SD_WAIT: begin
                if (sd_byte_cnt < CARD_SIZE[10:0]) begin
                    sd_write_data  <= 8'd0;
                    sd_write_valid <= 1'b1;
                    sd_byte_cnt    <= sd_byte_cnt + 1'b1;
                end
                if (sd_cmd_done) begin
                    resp_status <= RESP_204;
                    state       <= S_RESP_DONE;
                end
            end

            // ============================================================
            //  KV_LIST — scan card range, emit found entries
            // ============================================================

            // Advance to next card in scan window
            S_LST_NEXT: begin
                if (scan_limit == 16'd0 || scan_card >= scan_end) begin
                    sentinel_cnt <= 2'd0;
                    state        <= S_LST_SENTINEL;
                end else begin
                    state <= S_LST_SD_START;
                end
            end

            // Start SD read (first 2 bytes for magic check)
            S_LST_SD_START: begin
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= scan_baddr;
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_LST_SD_WAIT;
            end

            // Wait for first byte
            S_LST_SD_WAIT: begin
                if (sd_read_valid) begin
                    magic_byte0 <= sd_read_data;
                    sd_byte_cnt <= 11'd1;
                    state       <= S_LST_CHECK_MAGIC0;
                end
                if (sd_cmd_done && !sd_read_valid) begin
                    // No data — skip this card
                    scan_card <= scan_card + 22'd1;
                    state     <= S_LST_NEXT;
                end
            end

            // Check magic byte 0
            S_LST_CHECK_MAGIC0: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    if (magic_byte0 == MAGIC_LO && sd_read_data == MAGIC_HI) begin
                        // Valid — read full card
                        state <= S_LST_SD_FULL;
                    end else begin
                        // Invalid — skip, drain remaining SD data
                        scan_card <= scan_card + 22'd1;
                        state     <= S_LST_CHECK_MAGIC1;
                    end
                end
            end

            // Drain remaining bytes from invalid card
            S_LST_CHECK_MAGIC1: begin
                if (sd_read_valid)
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                if (sd_cmd_done) begin
                    state <= S_LST_NEXT;
                end
            end

            // Continue reading full card — wait for sd_cmd_done
            S_LST_SD_FULL: begin
                // Count incoming bytes
                if (sd_read_valid)
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                if (sd_cmd_done) begin
                    // Full card received — emit response header
                    lst_payload_len <= sd_byte_cnt;
                    hdr_cnt         <= 3'd0;
                    state           <= S_LST_STREAM_HDR;
                end
            end

            // Emit [card_ord:u32 LE][payload_len:u16 LE]
            S_LST_STREAM_HDR: begin
                resp_valid <= 1'b1;
                case (hdr_cnt)
                    3'd0: resp_data <= scan_card[7:0];
                    3'd1: resp_data <= scan_card[15:8];
                    3'd2: resp_data <= {2'b00, scan_card[21:16]};
                    3'd3: resp_data <= 8'd0;
                    3'd4: resp_data <= lst_payload_len[7:0];
                    3'd5: begin
                        resp_data <= {5'd0, lst_payload_len[10:8]};
                        state     <= S_LST_STREAM;
                    end
                    default: ;
                endcase
                hdr_cnt <= hdr_cnt + 3'd1;
            end

            // Stream card payload (already read — would need a buffer;
            // simplified: re-read from SD)
            S_LST_STREAM: begin
                // For iCE40 area efficiency we re-read the card for streaming.
                // Start a second SD read.
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= scan_baddr;
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_LST_SD_FULL_WAIT;
            end

            S_LST_SD_FULL_WAIT: begin
                if (sd_read_valid) begin
                    resp_data   <= sd_read_data;
                    resp_valid  <= 1'b1;
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                end
                if (sd_cmd_done) begin
                    scan_card  <= scan_card + 22'd1;
                    scan_limit <= scan_limit - 16'd1;
                    state      <= S_LST_NEXT;
                end
            end

            // Emit 0xFFFFFFFF end sentinel (4 bytes LE)
            S_LST_SENTINEL: begin
                resp_data  <= 8'hFF;
                resp_valid <= 1'b1;
                if (sentinel_cnt == 2'd3) begin
                    state <= S_LST_DONE;
                end
                sentinel_cnt <= sentinel_cnt + 2'd1;
            end

            S_LST_DONE: begin
                resp_status <= RESP_200;
                resp_done   <= 1'b1;
                state       <= S_IDLE;
            end

            // ============================================================
            //  KV_MGET — multi-get from list of card ordinals
            // ============================================================

            // Load MGET body (list of u32 card ordinals)
            S_MG_LOAD_BODY: begin
                if (mget_valid) begin
                    mget_buf[mget_wr_ptr] <= mget_data;
                    mget_wr_ptr <= mget_wr_ptr + 8'd1;
                end
                if (mget_done) begin
                    mget_len <= mget_wr_ptr;
                    mget_ptr <= 8'd0;
                    state    <= S_MG_NEXT;
                end
            end

            // Parse next u32 card ordinal from mget_buf
            S_MG_NEXT: begin
                if (mget_ptr + 8'd3 < mget_len) begin
                    // Read 4 bytes LE into card ordinal (only 22 bits used)
                    mget_card_ord <= {mget_buf[mget_ptr + 8'd2][5:0],
                                      mget_buf[mget_ptr + 8'd1],
                                      mget_buf[mget_ptr]};
                    mget_ptr      <= mget_ptr + 8'd4;
                    state         <= S_MG_SD_START;
                end else begin
                    sentinel_cnt <= 2'd0;
                    state        <= S_MG_SENTINEL;
                end
            end

            // Start SD read for this card
            S_MG_SD_START: begin
                card_r        <= mget_card_ord;
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= {pack_r, mget_card_ord, 2'b00};
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_MG_SD_WAIT;
            end

            // Wait for first byte
            S_MG_SD_WAIT: begin
                if (sd_read_valid) begin
                    magic_byte0 <= sd_read_data;
                    sd_byte_cnt <= 11'd1;
                    state       <= S_MG_CHECK_MAGIC0;
                end
                if (sd_cmd_done && !sd_read_valid) begin
                    state <= S_MG_NEXT;
                end
            end

            // Check magic byte 1
            S_MG_CHECK_MAGIC0: begin
                if (sd_read_valid) begin
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                    if (magic_byte0 == MAGIC_LO && sd_read_data == MAGIC_HI) begin
                        is_existing <= 1'b1;
                        state       <= S_MG_CHECK_MAGIC1;
                    end else begin
                        is_existing <= 1'b0;
                        state       <= S_MG_CHECK_MAGIC1;
                    end
                end
            end

            // Drain remaining bytes, then stream or skip
            S_MG_CHECK_MAGIC1: begin
                if (sd_read_valid)
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                if (sd_cmd_done) begin
                    if (is_existing) begin
                        hdr_cnt         <= 3'd0;
                        lst_payload_len <= sd_byte_cnt;
                        state           <= S_MG_STREAM_HDR;
                    end else begin
                        state <= S_MG_NEXT;
                    end
                end
            end

            // Emit [card_ord:u32 LE][payload_len:u16 LE]
            S_MG_STREAM_HDR: begin
                resp_valid <= 1'b1;
                case (hdr_cnt)
                    3'd0: resp_data <= mget_card_ord[7:0];
                    3'd1: resp_data <= mget_card_ord[15:8];
                    3'd2: resp_data <= {2'b00, mget_card_ord[21:16]};
                    3'd3: resp_data <= 8'd0;
                    3'd4: resp_data <= lst_payload_len[7:0];
                    3'd5: begin
                        resp_data <= {5'd0, lst_payload_len[10:8]};
                        state     <= S_MG_STREAM;
                    end
                    default: ;
                endcase
                hdr_cnt <= hdr_cnt + 3'd1;
            end

            // Re-read and stream full card
            S_MG_STREAM: begin
                sd_cmd_op     <= SD_READ;
                sd_block_addr <= {pack_r, mget_card_ord, 2'b00};
                sd_cmd_start  <= 1'b1;
                sd_byte_cnt   <= 11'd0;
                state         <= S_MG_STREAM_WAIT;
            end

            // Receive re-read bytes, stream to response
            S_MG_STREAM_WAIT: begin
                if (sd_read_valid) begin
                    resp_data   <= sd_read_data;
                    resp_valid  <= 1'b1;
                    sd_byte_cnt <= sd_byte_cnt + 1'b1;
                end
                if (sd_cmd_done) begin
                    state <= S_MG_NEXT;
                end
            end

            // Emit 0xFFFFFFFF sentinel
            S_MG_SENTINEL: begin
                resp_data  <= 8'hFF;
                resp_valid <= 1'b1;
                if (sentinel_cnt == 2'd3) begin
                    state <= S_MG_DONE;
                end
                sentinel_cnt <= sentinel_cnt + 2'd1;
            end

            S_MG_DONE: begin
                resp_status <= RESP_200;
                resp_done   <= 1'b1;
                state       <= S_IDLE;
            end

            // ============================================================
            //  RESP_DONE — generic completion state
            // ============================================================
            S_RESP_DONE: begin
                resp_done <= 1'b1;
                state     <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule
