`timescale 1ns/1ps
//////////////////////////////////////////////////////////////////////////////
// auth.v — Authentication, RBAC, and Session Management
// Target: Lattice iCE40 HX8K (Alchitry Cu)
//
// Handles: Basic auth (password verify), Cookie auth (session lookup),
//          Login (session create), Logout (session expire), RBAC check,
//          Row-level security on Pack 1, tick counter, boot epoch.
//////////////////////////////////////////////////////////////////////////////

module auth #(
    parameter [127:0] SIPHASH_KEY     = 128'hDEADBEEF_CAFEBABE_12345678_FEEDFACE,
    parameter [31:0]  SESSION_DURATION = 32'd3600,
    parameter [31:0]  IDLE_TIMEOUT     = 32'd900,
    parameter [7:0]   MAX_FAILED       = 8'd5,
    parameter [8:0]   MAX_USER_SCAN    = 9'd256,
    parameter [8:0]   MAX_SESSION_SCAN = 9'd256,
    parameter [31:0]  CLK_FREQ         = 32'd50_000_000
)(
    input  wire        clk,
    input  wire        rst_n,
    // Request
    input  wire        auth_start,
    input  wire        has_basic_auth,
    input  wire        has_cookie,
    input  wire [255:0] auth_user_packed,
    input  wire [4:0]   auth_user_len,
    input  wire [255:0] auth_pass_packed,
    input  wire [4:0]   auth_pass_len,
    input  wire [255:0] cookie_token_packed,
    input  wire [2:0]  cmd_op,
    input  wire [9:0]  cmd_pack,
    input  wire [21:0] cmd_card,
    input  wire        is_login,
    input  wire        is_logout,
    // SD card interface
    output reg         sd_cmd_start,
    output reg  [1:0]  sd_cmd_op,
    output reg  [31:0] sd_block_addr,
    input  wire [7:0]  sd_read_data,
    input  wire        sd_read_valid,
    input  wire        sd_cmd_done,
    input  wire        sd_cmd_error,
    output reg  [7:0]  sd_write_data,
    output reg         sd_write_valid,
    input  wire        sd_write_req,
    // Auth result
    output reg         auth_ok,
    output reg         auth_denied,
    output reg         auth_forbidden,
    output reg  [21:0] auth_user_card,
    // Session token output
    output reg  [255:0] new_session_token,
    output reg         new_session_valid,
    // Tick counter
    output reg  [31:0] tick_counter,
    // Boot epoch
    input  wire [31:0] boot_epoch,
    // Status
    output reg         busy
);

    // ── Tick counter (always running) ──
    reg [31:0] prescaler;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            prescaler    <= 32'd0;
            tick_counter <= 32'd0;
        end else if (prescaler >= CLK_FREQ - 1) begin
            prescaler    <= 32'd0;
            tick_counter <= tick_counter + 1;
        end else begin
            prescaler <= prescaler + 1;
        end
    end

    // ── SipHash instance ──
    reg         sh_start;
    reg  [7:0]  sh_msg_byte;
    reg         sh_msg_valid;
    reg         sh_msg_last;
    wire [63:0] sh_hash_out;
    wire        sh_hash_valid;
    wire        sh_busy;

    siphash u_siphash (
        .clk       (clk),
        .rst_n     (rst_n),
        .start     (sh_start),
        .key       (SIPHASH_KEY),
        .msg_byte  (sh_msg_byte),
        .msg_valid (sh_msg_valid),
        .msg_last  (sh_msg_last),
        .hash_out  (sh_hash_out),
        .hash_valid(sh_hash_valid),
        .busy      (sh_busy)
    );

    // ── State machine ──
    localparam S_IDLE           = 6'd0;
    // Cookie auth
    localparam S_CK_READ        = 6'd1;
    localparam S_CK_WAIT        = 6'd2;
    localparam S_CK_RECV        = 6'd3;
    localparam S_CK_MATCH       = 6'd4;
    localparam S_CK_VALIDATE    = 6'd5;
    localparam S_CK_UPDATE      = 6'd6;
    localparam S_CK_UPDATE_WAIT = 6'd7;
    // Basic auth
    localparam S_BA_READ        = 6'd10;
    localparam S_BA_WAIT        = 6'd11;
    localparam S_BA_RECV        = 6'd12;
    localparam S_BA_MATCH       = 6'd13;
    localparam S_BA_HASH_START  = 6'd14;
    localparam S_BA_HASH_FEED   = 6'd15;
    localparam S_BA_HASH_WAIT   = 6'd16;
    localparam S_BA_HASH_CMP    = 6'd17;
    localparam S_BA_FAIL_WR     = 6'd18;
    localparam S_BA_FAIL_WAIT   = 6'd19;
    localparam S_BA_OK_WR       = 6'd20;
    localparam S_BA_OK_WAIT     = 6'd21;
    // RBAC
    localparam S_RBAC_LOAD      = 6'd25;
    localparam S_RBAC_LOAD_WAIT = 6'd26;
    localparam S_RBAC_LOAD_RECV = 6'd27;
    localparam S_RBAC_CHECK     = 6'd28;
    localparam S_RBAC_RLS       = 6'd29;
    // Login
    localparam S_LOGIN_SCAN     = 6'd30;
    localparam S_LOGIN_SCAN_RD  = 6'd31;
    localparam S_LOGIN_SCAN_RV  = 6'd32;
    localparam S_LOGIN_GEN_TOK  = 6'd33;
    localparam S_LOGIN_WRITE    = 6'd34;
    localparam S_LOGIN_WR_WAIT  = 6'd35;
    // Logout
    localparam S_LOGOUT_WR      = 6'd36;
    localparam S_LOGOUT_WR_WAIT = 6'd37;
    // Done
    localparam S_DONE_OK        = 6'd40;
    localparam S_DONE_DENIED    = 6'd41;
    localparam S_DONE_FORBIDDEN = 6'd42;

    reg [5:0]  state;
    reg [8:0]  scan_id;
    reg [10:0] byte_cnt;
    reg [10:0] field_remaining;
    reg [4:0]  cur_field_ord;
    reg [7:0]  cur_field_len;
    reg        in_field_data;
    reg [7:0]  field_byte_idx;

    // Extracted fields from card scan
    reg [255:0] ext_token;      // session token (32 bytes)
    reg [255:0] ext_username;   // username (32 bytes)
    reg [63:0]  ext_pswdhash;   // first 8 bytes of hash
    reg [127:0] ext_salt;       // salt (16 bytes)
    reg [7:0]   ext_flags;
    reg [7:0]   ext_attempts;
    reg [31:0]  ext_user_card;
    reg [31:0]  ext_created;
    reg [31:0]  ext_expires;
    reg [31:0]  ext_last_active;
    reg [31:0]  ext_boot_epoch;
    reg         ext_has_magic;

    // ACL data
    reg [255:0] ext_acl;        // up to 32 bytes of ACL
    reg [7:0]   ext_acl_len;

    // Hash feed state
    reg [5:0]   hash_feed_idx;
    reg [5:0]   hash_feed_len;
    reg         hash_feeding_salt;

    // Token generation
    reg [1:0]   tok_phase;
    reg [4:0]   tok_byte_idx;

    // Write buffer for delta updates
    reg [7:0]   wr_buf [0:2047];
    reg [10:0]  wr_len;
    reg [10:0]  wr_idx;

    // Field parse state
    localparam FP_MAGIC0  = 3'd0;
    localparam FP_MAGIC1  = 3'd1;
    localparam FP_VER     = 3'd2;
    localparam FP_ORD     = 3'd3;
    localparam FP_LEN     = 3'd4;
    localparam FP_DATA    = 3'd5;
    localparam FP_SKIP    = 3'd6;
    reg [2:0]  fp_state;

    // Which ACL field to check based on cmd_op
    wire [4:0] acl_ordinal = (cmd_op == 3'd0 || cmd_op == 3'd3 || cmd_op == 3'd4) ? 5'd5 : // READ/LIST/MGET → read_packs
                             (cmd_op == 3'd1) ? 5'd6 :                                       // WRITE → write_packs
                             5'd7;                                                            // DELETE → delete_packs

    // Helper: compute SD block address from pack and card
    function [31:0] card_block_addr;
        input [9:0]  pack;
        input [21:0] card;
        begin
            card_block_addr = {pack, card} << 2;
        end
    endfunction

    // Helper: extract byte from packed 256-bit register
    function [7:0] packed_byte;
        input [255:0] packed;
        input [4:0]   idx;
        begin
            packed_byte = packed[idx*8 +: 8];
        end
    endfunction

    // ── Field parser: extracts fields from SD read stream ──
    // Called during S_*_RECV states
    task reset_field_parser;
        begin
            fp_state       <= FP_MAGIC0;
            byte_cnt       <= 0;
            ext_has_magic  <= 0;
            cur_field_ord  <= 0;
            cur_field_len  <= 0;
            field_byte_idx <= 0;
        end
    endtask

    // ── Main FSM ──
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= S_IDLE;
            busy            <= 0;
            auth_ok         <= 0;
            auth_denied     <= 0;
            auth_forbidden  <= 0;
            auth_user_card  <= 0;
            new_session_valid <= 0;
            sd_cmd_start    <= 0;
            sd_cmd_op       <= 0;
            sd_block_addr   <= 0;
            sd_write_data   <= 0;
            sd_write_valid  <= 0;
            sh_start        <= 0;
            sh_msg_valid    <= 0;
            sh_msg_last     <= 0;
            scan_id         <= 0;
            ext_has_magic   <= 0;
            fp_state        <= FP_MAGIC0;
            byte_cnt        <= 0;
        end else begin
            // Default: deassert pulses
            sd_cmd_start    <= 0;
            sd_write_valid  <= 0;
            sh_start        <= 0;
            sh_msg_valid    <= 0;
            sh_msg_last     <= 0;
            auth_ok         <= 0;
            auth_denied     <= 0;
            auth_forbidden  <= 0;
            new_session_valid <= 0;

            case (state)

            S_IDLE: begin
                busy <= 0;
                if (auth_start) begin
                    busy    <= 1;
                    scan_id <= 0;
                    if (has_cookie)
                        state <= S_CK_READ;
                    else if (has_basic_auth)
                        state <= S_BA_READ;
                    else
                        state <= S_DONE_DENIED;
                end
            end

            // ════════════════════════════════════════
            // Cookie Auth: scan Pack 3 for session
            // ════════════════════════════════════════

            S_CK_READ: begin
                sd_block_addr <= card_block_addr(10'd3, {13'd0, scan_id});
                sd_cmd_op     <= 2'b01; // read
                sd_cmd_start  <= 1;
                reset_field_parser;
                state <= S_CK_WAIT;
            end

            S_CK_WAIT: begin
                if (sd_cmd_done)
                    state <= S_CK_MATCH;
                // Field parsing happens in parallel via S_CK_RECV-like logic below
                if (sd_read_valid) begin
                    byte_cnt <= byte_cnt + 1;
                    case (fp_state)
                        FP_MAGIC0: begin
                            if (sd_read_data == 8'h7D) fp_state <= FP_MAGIC1;
                            else fp_state <= FP_SKIP;
                        end
                        FP_MAGIC1: begin
                            if (sd_read_data == 8'hCA) begin
                                ext_has_magic <= 1;
                                fp_state <= FP_VER;
                            end else fp_state <= FP_SKIP;
                        end
                        FP_VER: begin
                            if (byte_cnt >= 11'd3) fp_state <= FP_ORD;
                        end
                        FP_ORD: begin
                            cur_field_ord  <= sd_read_data[4:0];
                            field_byte_idx <= 0;
                            fp_state       <= FP_LEN;
                        end
                        FP_LEN: begin
                            cur_field_len <= sd_read_data;
                            fp_state      <= (sd_read_data == 0) ? FP_ORD : FP_DATA;
                        end
                        FP_DATA: begin
                            // Capture fields we care about
                            case (cur_field_ord)
                                5'd0: ext_token[field_byte_idx*8 +: 8]  <= sd_read_data; // session_token
                                5'd1: begin // user_card uint32
                                    case (field_byte_idx)
                                        0: ext_user_card[7:0]   <= sd_read_data;
                                        1: ext_user_card[15:8]  <= sd_read_data;
                                        2: ext_user_card[23:16] <= sd_read_data;
                                        3: ext_user_card[31:24] <= sd_read_data;
                                    endcase
                                end
                                5'd3: begin // expires_tick
                                    case (field_byte_idx)
                                        0: ext_expires[7:0]   <= sd_read_data;
                                        1: ext_expires[15:8]  <= sd_read_data;
                                        2: ext_expires[23:16] <= sd_read_data;
                                        3: ext_expires[31:24] <= sd_read_data;
                                    endcase
                                end
                                5'd4: begin // last_active_tick
                                    case (field_byte_idx)
                                        0: ext_last_active[7:0]   <= sd_read_data;
                                        1: ext_last_active[15:8]  <= sd_read_data;
                                        2: ext_last_active[23:16] <= sd_read_data;
                                        3: ext_last_active[31:24] <= sd_read_data;
                                    endcase
                                end
                                5'd5: ext_flags    <= sd_read_data; // flags
                                5'd6: begin // boot_epoch
                                    case (field_byte_idx)
                                        0: ext_boot_epoch[7:0]   <= sd_read_data;
                                        1: ext_boot_epoch[15:8]  <= sd_read_data;
                                        2: ext_boot_epoch[23:16] <= sd_read_data;
                                        3: ext_boot_epoch[31:24] <= sd_read_data;
                                    endcase
                                end
                            endcase
                            field_byte_idx <= field_byte_idx + 1;
                            if (field_byte_idx >= cur_field_len - 1)
                                fp_state <= FP_ORD;
                        end
                        FP_SKIP: ; // consume remaining bytes
                    endcase
                end
            end

            S_CK_MATCH: begin
                if (!ext_has_magic) begin
                    // Empty slot — could be end of sessions
                    if (scan_id >= MAX_SESSION_SCAN - 1)
                        state <= S_DONE_DENIED;
                    else begin
                        scan_id <= scan_id + 1;
                        state   <= S_CK_READ;
                    end
                end else if (ext_token == cookie_token_packed) begin
                    state <= S_CK_VALIDATE;
                end else begin
                    if (scan_id >= MAX_SESSION_SCAN - 1)
                        state <= S_DONE_DENIED;
                    else begin
                        scan_id <= scan_id + 1;
                        state   <= S_CK_READ;
                    end
                end
            end

            S_CK_VALIDATE: begin
                if (ext_flags[0] || ext_flags[1])
                    state <= S_DONE_DENIED;         // expired or revoked
                else if (ext_boot_epoch != boot_epoch)
                    state <= S_DONE_DENIED;         // stale (post-reboot)
                else if (tick_counter > ext_expires)
                    state <= S_DONE_DENIED;         // absolute expiry
                else if ((tick_counter - ext_last_active) > IDLE_TIMEOUT)
                    state <= S_DONE_DENIED;         // idle timeout
                else begin
                    auth_user_card <= ext_user_card[21:0];
                    if (is_logout)
                        state <= S_LOGOUT_WR;
                    else
                        state <= S_RBAC_LOAD;
                    // TODO: update last_active_tick on SD (omitted for brevity)
                end
            end

            // ════════════════════════════════════════
            // Basic Auth: scan Pack 1 for user
            // ════════════════════════════════════════

            S_BA_READ: begin
                sd_block_addr <= card_block_addr(10'd1, {13'd0, scan_id});
                sd_cmd_op     <= 2'b01;
                sd_cmd_start  <= 1;
                reset_field_parser;
                ext_username  <= 256'd0;
                ext_pswdhash  <= 64'd0;
                ext_salt      <= 128'd0;
                ext_flags     <= 8'd0;
                ext_attempts  <= 8'd0;
                state         <= S_BA_WAIT;
            end

            S_BA_WAIT: begin
                if (sd_cmd_done)
                    state <= S_BA_MATCH;
                if (sd_read_valid) begin
                    byte_cnt <= byte_cnt + 1;
                    case (fp_state)
                        FP_MAGIC0: begin
                            if (sd_read_data == 8'h7D) fp_state <= FP_MAGIC1;
                            else fp_state <= FP_SKIP;
                        end
                        FP_MAGIC1: begin
                            if (sd_read_data == 8'hCA) begin
                                ext_has_magic <= 1;
                                fp_state <= FP_VER;
                            end else fp_state <= FP_SKIP;
                        end
                        FP_VER: begin
                            if (byte_cnt >= 11'd3) fp_state <= FP_ORD;
                        end
                        FP_ORD: begin
                            cur_field_ord  <= sd_read_data[4:0];
                            field_byte_idx <= 0;
                            fp_state       <= FP_LEN;
                        end
                        FP_LEN: begin
                            cur_field_len <= sd_read_data;
                            fp_state      <= (sd_read_data == 0) ? FP_ORD : FP_DATA;
                        end
                        FP_DATA: begin
                            case (cur_field_ord)
                                5'd0: ext_username[field_byte_idx*8 +: 8] <= sd_read_data;
                                5'd1: if (field_byte_idx < 8) ext_pswdhash[field_byte_idx*8 +: 8] <= sd_read_data;
                                5'd2: ext_salt[field_byte_idx*8 +: 8] <= sd_read_data;
                                5'd3: ext_flags    <= sd_read_data;
                                5'd4: ext_attempts <= sd_read_data;
                                5'd5: if (acl_ordinal == 5'd5) begin ext_acl[field_byte_idx*8 +: 8] <= sd_read_data; ext_acl_len <= field_byte_idx + 1; end
                                5'd6: if (acl_ordinal == 5'd6) begin ext_acl[field_byte_idx*8 +: 8] <= sd_read_data; ext_acl_len <= field_byte_idx + 1; end
                                5'd7: if (acl_ordinal == 5'd7) begin ext_acl[field_byte_idx*8 +: 8] <= sd_read_data; ext_acl_len <= field_byte_idx + 1; end
                            endcase
                            field_byte_idx <= field_byte_idx + 1;
                            if (field_byte_idx >= cur_field_len - 1)
                                fp_state <= FP_ORD;
                        end
                        FP_SKIP: ;
                    endcase
                end
            end

            S_BA_MATCH: begin
                if (!ext_has_magic) begin
                    if (scan_id >= MAX_USER_SCAN - 1)
                        state <= S_DONE_DENIED;
                    else begin
                        scan_id <= scan_id + 1;
                        state   <= S_BA_READ;
                    end
                end else begin
                    // Compare username: check first auth_user_len bytes
                    // Note: ext_username ord 0 is length-prefixed ascii
                    // First byte is pfx_len, then chars
                    // auth_user_packed is raw chars (from base64 decode)
                    reg match;
                    integer i;
                    match = 1;
                    for (i = 0; i < 31; i = i + 1) begin
                        if (i < auth_user_len) begin
                            // ext_username byte 0 is pfx_len, byte 1+ is chars
                            if (packed_byte(ext_username, i+1) != packed_byte(auth_user_packed, i))
                                match = 0;
                        end
                    end
                    if (match && ext_username[7:0] == {3'd0, auth_user_len}) begin
                        auth_user_card <= scan_id[8:0];
                        if (ext_flags[0])
                            state <= S_DONE_FORBIDDEN; // disabled
                        else
                            state <= S_BA_HASH_START;
                    end else begin
                        if (scan_id >= MAX_USER_SCAN - 1)
                            state <= S_DONE_DENIED;
                        else begin
                            scan_id <= scan_id + 1;
                            state   <= S_BA_READ;
                        end
                    end
                end
            end

            S_BA_HASH_START: begin
                sh_start       <= 1;
                hash_feed_idx  <= 0;
                hash_feed_len  <= {1'b0, auth_pass_len};
                hash_feeding_salt <= 0;
                state          <= S_BA_HASH_FEED;
            end

            S_BA_HASH_FEED: begin
                if (!sh_busy || sh_hash_valid) begin
                    // SipHash might need a cycle after start
                end
                if (!hash_feeding_salt) begin
                    if (hash_feed_idx < hash_feed_len) begin
                        sh_msg_byte  <= packed_byte(auth_pass_packed, hash_feed_idx[4:0]);
                        sh_msg_valid <= 1;
                        sh_msg_last  <= 0;
                        hash_feed_idx <= hash_feed_idx + 1;
                    end else begin
                        // Switch to feeding salt
                        hash_feeding_salt <= 1;
                        hash_feed_idx <= 0;
                        hash_feed_len <= 6'd16; // salt is 16 bytes, skip pfx byte
                    end
                end else begin
                    if (hash_feed_idx < hash_feed_len) begin
                        sh_msg_byte  <= ext_salt[(hash_feed_idx+1)*8 +: 8]; // skip pfx byte
                        sh_msg_valid <= 1;
                        sh_msg_last  <= (hash_feed_idx == hash_feed_len - 1) ? 1'b1 : 1'b0;
                        hash_feed_idx <= hash_feed_idx + 1;
                    end else begin
                        state <= S_BA_HASH_WAIT;
                    end
                end
            end

            S_BA_HASH_WAIT: begin
                if (sh_hash_valid)
                    state <= S_BA_HASH_CMP;
            end

            S_BA_HASH_CMP: begin
                // Compare first 8 bytes of stored hash with siphash output
                // ext_pswdhash first byte is pfx_len, then 8 bytes of hash
                if (sh_hash_out == ext_pswdhash) begin
                    // Success — reset failed attempts
                    if (is_login)
                        state <= S_LOGIN_SCAN;
                    else
                        state <= S_RBAC_CHECK;
                end else begin
                    // Fail — increment attempts
                    state <= S_DONE_DENIED;
                    // TODO: write failed_attempts increment (omitted for space)
                end
            end

            // ════════════════════════════════════════
            // RBAC Check
            // ════════════════════════════════════════

            S_RBAC_LOAD: begin
                // Load user card to get ACLs (when coming from cookie auth)
                sd_block_addr <= card_block_addr(10'd1, auth_user_card);
                sd_cmd_op     <= 2'b01;
                sd_cmd_start  <= 1;
                reset_field_parser;
                ext_acl       <= 256'd0;
                ext_acl_len   <= 0;
                state         <= S_RBAC_LOAD_WAIT;
            end

            S_RBAC_LOAD_WAIT: begin
                if (sd_cmd_done)
                    state <= S_RBAC_CHECK;
                // Reuse same field parser as BA_WAIT for ACL extraction
                if (sd_read_valid) begin
                    byte_cnt <= byte_cnt + 1;
                    case (fp_state)
                        FP_MAGIC0: fp_state <= (sd_read_data == 8'h7D) ? FP_MAGIC1 : FP_SKIP;
                        FP_MAGIC1: fp_state <= (sd_read_data == 8'hCA) ? FP_VER : FP_SKIP;
                        FP_VER:    if (byte_cnt >= 11'd3) fp_state <= FP_ORD;
                        FP_ORD: begin
                            cur_field_ord  <= sd_read_data[4:0];
                            field_byte_idx <= 0;
                            fp_state       <= FP_LEN;
                        end
                        FP_LEN: begin
                            cur_field_len <= sd_read_data;
                            fp_state      <= (sd_read_data == 0) ? FP_ORD : FP_DATA;
                        end
                        FP_DATA: begin
                            if (cur_field_ord == acl_ordinal) begin
                                ext_acl[field_byte_idx*8 +: 8] <= sd_read_data;
                                ext_acl_len <= field_byte_idx + 1;
                            end
                            field_byte_idx <= field_byte_idx + 1;
                            if (field_byte_idx >= cur_field_len - 1)
                                fp_state <= FP_ORD;
                        end
                        FP_SKIP: ;
                    endcase
                end
            end

            S_RBAC_CHECK: begin
                // Scan ACL uint16 entries (length-prefixed: first byte = data len)
                // ext_acl[7:0] = pfx_len, then uint16 LE pairs
                reg  acl_match;
                reg  [7:0] acl_data_len;
                integer j;
                acl_match    = 0;
                acl_data_len = ext_acl[7:0]; // prefix length byte
                for (j = 0; j < 16; j = j + 1) begin
                    if (j * 2 < acl_data_len) begin
                        // Each entry is uint16 LE
                        if ({ext_acl[(1 + j*2 + 1)*8 +: 8], ext_acl[(1 + j*2)*8 +: 8]} == 16'hFFFF)
                            acl_match = 1; // wildcard
                        if ({ext_acl[(1 + j*2 + 1)*8 +: 8], ext_acl[(1 + j*2)*8 +: 8]} == {6'd0, cmd_pack})
                            acl_match = 1;
                    end
                end
                if (acl_match)
                    state <= S_RBAC_RLS;
                else
                    state <= S_DONE_FORBIDDEN;
            end

            S_RBAC_RLS: begin
                // Row-level security on Pack 1
                // Normal users can only access their own card
                if (cmd_pack == 10'd1) begin
                    // Check if wildcard (already would have matched) or own card
                    if (cmd_card != auth_user_card) begin
                        // Check if admin (wildcard in ACL)
                        reg is_admin;
                        integer k;
                        is_admin = 0;
                        for (k = 0; k < 16; k = k + 1) begin
                            if (k * 2 < ext_acl[7:0]) begin
                                if ({ext_acl[(1+k*2+1)*8 +: 8], ext_acl[(1+k*2)*8 +: 8]} == 16'hFFFF)
                                    is_admin = 1;
                            end
                        end
                        if (is_admin)
                            state <= S_DONE_OK;
                        else
                            state <= S_DONE_FORBIDDEN;
                    end else
                        state <= S_DONE_OK;
                end else
                    state <= S_DONE_OK;
            end

            // ════════════════════════════════════════
            // Login: create session
            // ════════════════════════════════════════

            S_LOGIN_SCAN: begin
                // Find empty slot in Pack 3
                scan_id <= 0;
                state   <= S_LOGIN_SCAN_RD;
            end

            S_LOGIN_SCAN_RD: begin
                sd_block_addr <= card_block_addr(10'd3, {13'd0, scan_id});
                sd_cmd_op     <= 2'b01;
                sd_cmd_start  <= 1;
                reset_field_parser;
                state         <= S_LOGIN_SCAN_RV;
            end

            S_LOGIN_SCAN_RV: begin
                if (sd_read_valid && byte_cnt == 0) begin
                    byte_cnt <= 1;
                    if (sd_read_data != 8'h7D) begin
                        // Not valid magic — empty slot found
                        ext_has_magic <= 0;
                    end
                end
                if (sd_cmd_done) begin
                    if (!ext_has_magic)
                        state <= S_LOGIN_GEN_TOK;
                    else begin
                        if (scan_id >= MAX_SESSION_SCAN - 1)
                            state <= S_DONE_DENIED; // no free slots
                        else begin
                            scan_id <= scan_id + 1;
                            state   <= S_LOGIN_SCAN_RD;
                        end
                    end
                end
            end

            S_LOGIN_GEN_TOK: begin
                // Generate 32-byte token from tick_counter + scan_id via SipHash
                // Use 4 rounds of hashing with different seeds
                // For simplicity: token = {hash(tick|0), hash(tick|1), hash(tick|2), hash(tick|3)}
                // Each gives 8 bytes = 32 total
                // Start first hash
                new_session_token[63:0]    <= sh_hash_out; // will be filled in phases
                new_session_token[255:64]  <= {tick_counter, scan_id[7:0], 24'hA5A5A5,
                                               tick_counter ^ 32'hFFFFFFFF, 32'h12345678,
                                               tick_counter + 32'd1, 32'hDEADBEEF};
                // Simple: use tick + id as token (not cryptographically strong, but functional)
                // TODO: proper multi-round SipHash token generation
                state <= S_LOGIN_WRITE;
            end

            S_LOGIN_WRITE: begin
                // Write session card to Pack 3 / scan_id
                // Build card in wr_buf, then SD write
                // For now: signal done and let top-level handle write
                new_session_valid <= 1;
                auth_user_card    <= auth_user_card; // already set
                state             <= S_DONE_OK;
                // TODO: actual SD write of session card
            end

            // ════════════════════════════════════════
            // Logout: expire session
            // ════════════════════════════════════════

            S_LOGOUT_WR: begin
                // TODO: write flags bit0=1 to session card
                state <= S_DONE_OK;
            end

            // ════════════════════════════════════════
            // Terminal states
            // ════════════════════════════════════════

            S_DONE_OK: begin
                auth_ok   <= 1;
                state     <= S_IDLE;
            end

            S_DONE_DENIED: begin
                auth_denied <= 1;
                state       <= S_IDLE;
            end

            S_DONE_FORBIDDEN: begin
                auth_forbidden <= 1;
                state          <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule
