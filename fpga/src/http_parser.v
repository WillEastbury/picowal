`timescale 1ns/1ps
module http_parser (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  rx_data,
    input  wire        rx_valid,
    input  wire        rx_done,
    output reg         rx_consumed,
    // Parsed output
    output reg         cmd_valid,
    output reg  [2:0]  cmd_op,        // 0=READ,1=WRITE,2=DELETE,3=LIST,4=MGET,5=LOGIN,6=LOGOUT,7=PASSWD
    output reg  [9:0]  cmd_pack,
    output reg  [21:0] cmd_card,
    output reg         cmd_is_ui,     // GET /
    output reg         cmd_is_status, // GET /status
    output reg  [21:0] list_start,
    output reg  [15:0] list_limit,
    output reg  [15:0] content_length,
    output reg         has_basic_auth,
    output reg         has_cookie,
    output reg  [7:0]  body_data,
    output reg         body_valid,
    output reg         body_done,
    output reg         parse_error,
    output reg         busy,
    // Auth credential registers (directly accessible by auth module)
    output reg  [255:0] auth_user_packed, // up to 32 bytes packed
    output reg  [4:0]   auth_user_len,
    output reg  [255:0] auth_pass_packed, // up to 32 bytes packed
    output reg  [4:0]   auth_pass_len,
    // Cookie token (32 bytes)
    output reg  [255:0] cookie_token_packed // 32 bytes packed
);

    // ================================================================
    // ASCII constants
    // ================================================================
    localparam [7:0] CH_SP    = 8'h20, // ' '
                     CH_CR    = 8'h0D, // '\r'
                     CH_LF    = 8'h0A, // '\n'
                     CH_SLASH = 8'h2F, // '/'
                     CH_QMARK = 8'h3F, // '?'
                     CH_AMP   = 8'h26, // '&'
                     CH_EQ    = 8'h3D, // '='
                     CH_COLON = 8'h3A, // ':'
                     CH_PLUS  = 8'h2B, // '+'
                     CH_UNDER = 8'h5F, // '_'
                     CH_0     = 8'h30,
                     CH_9     = 8'h39,
                     CH_A     = 8'h41,
                     CH_F     = 8'h46,
                     CH_Z     = 8'h5A,
                     CH_a     = 8'h61,
                     CH_f     = 8'h66,
                     CH_z     = 8'h7A;

    // ================================================================
    // FSM states
    // ================================================================
    localparam [4:0] S_IDLE       = 5'd0,
                     S_METHOD     = 5'd1,
                     S_URL        = 5'd2,
                     S_URL_PACK   = 5'd3,
                     S_URL_CARD   = 5'd4,
                     S_URL_SUB    = 5'd5,
                     S_URL_STATUS = 5'd6,
                     S_URL_LOGIN  = 5'd7,
                     S_QUERY_KEY  = 5'd8,
                     S_QUERY_VAL  = 5'd9,
                     S_SKIP_VER   = 5'd10,
                     S_HEADERS    = 5'd11,
                     S_HDR_SKIP   = 5'd12,
                     S_HDR_AUTH   = 5'd13,
                     S_BASE64     = 5'd14,
                     S_B64_EMIT   = 5'd15,
                     S_HDR_COO    = 5'd16,
                     S_HEX        = 5'd17,
                     S_HDR_CL     = 5'd18,
                     S_HDR_CL_VAL = 5'd19,
                     S_BODY       = 5'd20,
                     S_DONE       = 5'd21,
                     S_ERROR      = 5'd22;

    // Method encoding
    localparam [1:0] M_GET  = 2'd0,
                     M_PUT  = 2'd1,
                     M_POST = 2'd2,
                     M_DEL  = 2'd3;

    // cmd_op encoding
    localparam [2:0] OP_READ   = 3'd0,
                     OP_WRITE  = 3'd1,
                     OP_DELETE = 3'd2,
                     OP_LIST   = 3'd3,
                     OP_MGET   = 3'd4,
                     OP_LOGIN  = 3'd5,
                     OP_LOGOUT = 3'd6,
                     OP_PASSWD = 3'd7;

    // ================================================================
    // Internal registers
    // ================================================================
    reg [4:0]  state;

    // Method parsing
    reg [1:0]  method;
    reg [2:0]  meth_cnt;       // bytes accumulated (0-6)
    reg [47:0] meth_shift;     // 6-byte shift register (newest at [7:0])

    // URL parsing
    reg        has_pack;
    reg        has_card;
    reg        is_mget;
    reg        is_login;
    reg        is_logout;
    reg        is_passwd;
    reg        is_ui;
    reg        is_status;
    reg [21:0] dec_accum;      // decimal accumulator
    reg [2:0]  url_pos;        // position counter within URL states
    reg [3:0]  sub_cnt;        // sub-state byte counter
    reg        sub_is_m;       // S_URL_SUB: 1=matching mget, 0=matching pass
    reg        login_is_out;   // S_URL_LOGIN: 0=login, 1=logout

    // Query parsing
    reg [39:0] qkey_shift;     // 5-byte shift register for key matching
    reg [2:0]  qkey_cnt;       // bytes in key so far

    // Header parsing
    reg        hdr_saw_cr;     // saw CR (for \r\n detection)
    reg [2:0]  hdr_pos;        // position within header start detection
    reg [7:0]  hdr_byte0;      // first byte of header line

    // Content-Length
    reg [15:0] cl_accum;
    reg [1:0]  cl_skip_phase;  // 0=scan ':', 1=skip SP, 2=done

    // Authorization header
    reg [2:0]  auth_skip_cnt;  // 0=scan 'B', 1-5=match "asic "

    // Cookie header
    reg [1:0]  cookie_match_pos; // 0=scan 's', 1=match 'i', 2='d', 3='='

    // Base64 decoding
    reg [1:0]  b64_phase;      // 0-3 within a quad
    reg [23:0] b64_buf;        // accumulated 24-bit buffer
    reg        b64_saw_colon;  // seen ':' separator in decoded output
    reg [4:0]  b64_user_cnt;   // bytes written to user
    reg [4:0]  b64_pass_cnt;   // bytes written to pass
    reg        b64_p2_pad;     // phase 2 was padding
    reg        b64_p3_pad;     // phase 3 was padding
    reg [7:0]  b64_out0;       // decoded byte 0 (buf[23:16])
    reg [7:0]  b64_out1;       // decoded byte 1 (buf[15:8])
    reg [7:0]  b64_out2;       // decoded byte 2 (buf[7:0])
    reg [1:0]  b64_emit_idx;   // emit sub-counter (0,1,2)
    reg [1:0]  b64_num_valid;  // valid output bytes this quad (1-3)

    // Hex decoding (cookie)
    reg [5:0]  hex_cnt;        // 0-63 hex char counter
    reg [3:0]  hex_hi_nib;     // stored high nibble

    // Body pass-through
    reg [15:0] body_remain;

    // ================================================================
    // Combinational: Base64 lookup
    // ================================================================
    reg  [5:0] b64_lookup;
    reg        b64_is_pad;
    reg        b64_is_valid;

    always @(*) begin
        b64_lookup   = 6'd0;
        b64_is_pad   = 1'b0;
        b64_is_valid = 1'b0;
        if (rx_data >= CH_A && rx_data <= CH_Z) begin        // 'A'-'Z' → 0-25
            b64_lookup   = rx_data[5:0] - 6'd1;
            b64_is_valid = 1'b1;
        end else if (rx_data >= CH_a && rx_data <= CH_z) begin // 'a'-'z' → 26-51
            b64_lookup   = {1'b0, rx_data[4:0]} + 6'd25;
            b64_is_valid = 1'b1;
        end else if (rx_data >= CH_0 && rx_data <= CH_9) begin // '0'-'9' → 52-61
            b64_lookup   = {2'b00, rx_data[3:0]} + 6'd52;
            b64_is_valid = 1'b1;
        end else if (rx_data == CH_PLUS) begin                 // '+' → 62
            b64_lookup   = 6'd62;
            b64_is_valid = 1'b1;
        end else if (rx_data == CH_SLASH) begin                // '/' → 63
            b64_lookup   = 6'd63;
            b64_is_valid = 1'b1;
        end else if (rx_data == CH_EQ) begin                   // '=' padding
            b64_is_pad   = 1'b1;
            b64_is_valid = 1'b1;
        end
    end

    // ================================================================
    // Combinational: Hex nibble lookup
    // ================================================================
    reg  [3:0] hex_nibble;
    reg        hex_valid_ch;

    always @(*) begin
        hex_nibble   = 4'd0;
        hex_valid_ch = 1'b0;
        if (rx_data >= CH_0 && rx_data <= CH_9) begin
            hex_nibble   = rx_data[3:0];
            hex_valid_ch = 1'b1;
        end else if (rx_data >= CH_a && rx_data <= CH_f) begin
            hex_nibble   = rx_data[3:0] + 4'd9;
            hex_valid_ch = 1'b1;
        end else if (rx_data >= CH_A && rx_data <= CH_F) begin
            hex_nibble   = rx_data[3:0] + 4'd9;
            hex_valid_ch = 1'b1;
        end
    end

    // ================================================================
    // Combinational: Decimal helpers
    // ================================================================
    wire is_digit = (rx_data >= CH_0) && (rx_data <= CH_9);

    // dec_accum * 10 = (dec_accum << 3) + (dec_accum << 1)
    wire [25:0] dec_x8   = {1'b0, dec_accum, 3'b000};
    wire [25:0] dec_x2   = {3'b000, dec_accum, 1'b0};
    wire [25:0] dec_x10  = dec_x8 + dec_x2;
    wire [21:0] dec_next = dec_x10[21:0] + {18'd0, rx_data[3:0]};

    // Combinational: Base64 emit byte selector
    wire [7:0] b64_emit_byte = (b64_emit_idx == 2'd0) ? b64_out0 :
                               (b64_emit_idx == 2'd1) ? b64_out1 : b64_out2;

    // cl_accum * 10
    wire [19:0] cl_x8  = {1'b0, cl_accum, 3'b000};
    wire [19:0] cl_x2  = {3'b000, cl_accum, 1'b0};
    wire [19:0] cl_x10 = cl_x8 + cl_x2;
    wire [15:0] cl_next = cl_x10[15:0] + {12'd0, rx_data[3:0]};

    // ================================================================
    // Main FSM
    // ================================================================
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= S_IDLE;
            rx_consumed        <= 1'b0;
            cmd_valid          <= 1'b0;
            cmd_op             <= 3'd0;
            cmd_pack           <= 10'd0;
            cmd_card           <= 22'd0;
            cmd_is_ui          <= 1'b0;
            cmd_is_status      <= 1'b0;
            list_start         <= 22'd0;
            list_limit         <= 16'd0;
            content_length     <= 16'd0;
            has_basic_auth     <= 1'b0;
            has_cookie         <= 1'b0;
            body_data          <= 8'd0;
            body_valid         <= 1'b0;
            body_done          <= 1'b0;
            parse_error        <= 1'b0;
            busy               <= 1'b0;
            auth_user_packed   <= 256'd0;
            auth_user_len      <= 5'd0;
            auth_pass_packed   <= 256'd0;
            auth_pass_len      <= 5'd0;
            cookie_token_packed <= 256'd0;
            method             <= 2'd0;
            meth_cnt           <= 3'd0;
            meth_shift         <= 48'd0;
            has_pack           <= 1'b0;
            has_card           <= 1'b0;
            is_mget            <= 1'b0;
            is_login           <= 1'b0;
            is_logout          <= 1'b0;
            is_passwd          <= 1'b0;
            is_ui              <= 1'b0;
            is_status          <= 1'b0;
            dec_accum          <= 22'd0;
            url_pos            <= 3'd0;
            sub_cnt            <= 4'd0;
            sub_is_m           <= 1'b0;
            login_is_out       <= 1'b0;
            qkey_shift         <= 40'd0;
            qkey_cnt           <= 3'd0;
            hdr_saw_cr         <= 1'b0;
            hdr_pos            <= 3'd0;
            hdr_byte0          <= 8'd0;
            cl_accum           <= 16'd0;
            cl_skip_phase      <= 2'd0;
            auth_skip_cnt      <= 3'd0;
            cookie_match_pos   <= 2'd0;
            b64_phase          <= 2'd0;
            b64_buf            <= 24'd0;
            b64_saw_colon      <= 1'b0;
            b64_user_cnt       <= 5'd0;
            b64_pass_cnt       <= 5'd0;
            b64_p2_pad         <= 1'b0;
            b64_p3_pad         <= 1'b0;
            b64_out0           <= 8'd0;
            b64_out1           <= 8'd0;
            b64_out2           <= 8'd0;
            b64_emit_idx       <= 2'd0;
            b64_num_valid      <= 2'd0;
            hex_cnt            <= 6'd0;
            hex_hi_nib         <= 4'd0;
            body_remain        <= 16'd0;
        end else begin
            // Default: clear pulse outputs
            cmd_valid   <= 1'b0;
            rx_consumed <= 1'b0;
            body_valid  <= 1'b0;
            body_done   <= 1'b0;

            case (state)

            // --------------------------------------------------------
            // S_IDLE: Wait for first valid byte of request
            // --------------------------------------------------------
            S_IDLE: begin
                if (rx_valid) begin
                    busy               <= 1'b1;
                    parse_error        <= 1'b0;
                    // Clear all parsed fields
                    cmd_op             <= 3'd0;
                    cmd_pack           <= 10'd0;
                    cmd_card           <= 22'd0;
                    cmd_is_ui          <= 1'b0;
                    cmd_is_status      <= 1'b0;
                    list_start         <= 22'd0;
                    list_limit         <= 16'd20;
                    content_length     <= 16'd0;
                    has_basic_auth     <= 1'b0;
                    has_cookie         <= 1'b0;
                    has_pack           <= 1'b0;
                    has_card           <= 1'b0;
                    is_mget            <= 1'b0;
                    is_login           <= 1'b0;
                    is_logout          <= 1'b0;
                    is_passwd          <= 1'b0;
                    is_ui              <= 1'b0;
                    is_status          <= 1'b0;
                    auth_user_packed   <= 256'd0;
                    auth_user_len      <= 5'd0;
                    auth_pass_packed   <= 256'd0;
                    auth_pass_len      <= 5'd0;
                    cookie_token_packed <= 256'd0;
                    b64_saw_colon      <= 1'b0;
                    b64_user_cnt       <= 5'd0;
                    b64_pass_cnt       <= 5'd0;
                    b64_phase          <= 2'd0;
                    b64_buf            <= 24'd0;
                    b64_p2_pad         <= 1'b0;
                    b64_p3_pad         <= 1'b0;
                    hex_cnt            <= 6'd0;
                    dec_accum          <= 22'd0;
                    cl_accum           <= 16'd0;
                    body_remain        <= 16'd0;
                    // Store first byte and go to METHOD
                    meth_shift         <= {40'd0, rx_data};
                    meth_cnt           <= 3'd1;
                    state              <= S_METHOD;
                end else if (rx_done) begin
                    rx_consumed <= 1'b1;
                end
            end

            // --------------------------------------------------------
            // S_METHOD: Accumulate method bytes, detect on space
            // --------------------------------------------------------
            S_METHOD: begin
                if (rx_valid) begin
                    if (rx_data == CH_SP) begin
                        // Space terminates method — identify it
                        case (meth_cnt)
                        3'd3: begin
                            if (meth_shift[23:0] == {8'h47, 8'h45, 8'h54}) begin      // "GET"
                                method  <= M_GET;
                                state   <= S_URL;
                                url_pos <= 3'd0;
                            end else if (meth_shift[23:0] == {8'h50, 8'h55, 8'h54}) begin // "PUT"
                                method  <= M_PUT;
                                state   <= S_URL;
                                url_pos <= 3'd0;
                            end else
                                state <= S_ERROR;
                        end
                        3'd4: begin
                            if (meth_shift[31:0] == {8'h50, 8'h4F, 8'h53, 8'h54}) begin // "POST"
                                method  <= M_POST;
                                state   <= S_URL;
                                url_pos <= 3'd0;
                            end else
                                state <= S_ERROR;
                        end
                        3'd6: begin
                            if (meth_shift[47:0] == {8'h44, 8'h45, 8'h4C, 8'h45, 8'h54, 8'h45}) begin // "DELETE"
                                method  <= M_DEL;
                                state   <= S_URL;
                                url_pos <= 3'd0;
                            end else
                                state <= S_ERROR;
                        end
                        default: state <= S_ERROR;
                        endcase
                    end else begin
                        meth_shift <= {meth_shift[39:0], rx_data};
                        meth_cnt   <= meth_cnt + 3'd1;
                        if (meth_cnt >= 3'd6)
                            state <= S_ERROR;
                    end
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL: Parse first bytes of URL path
            // --------------------------------------------------------
            S_URL: begin
                if (rx_valid) begin
                    case (url_pos)
                    3'd0: begin // Expect '/'
                        if (rx_data == CH_SLASH)
                            url_pos <= 3'd1;
                        else
                            state <= S_ERROR;
                    end
                    3'd1: begin // Second byte determines path type
                        if (rx_data == CH_SP || rx_data == 8'h48) begin // ' ' or 'H'
                            is_ui <= 1'b1;
                            state <= S_SKIP_VER;
                            hdr_saw_cr <= 1'b0;
                        end else if (rx_data == 8'h73) begin // 's' -> /status
                            sub_cnt <= 4'd0;
                            state   <= S_URL_STATUS;
                        end else if (is_digit) begin // digit -> pack number
                            dec_accum <= {18'd0, rx_data[3:0]};
                            state     <= S_URL_PACK;
                        end else if (rx_data == 8'h6C) begin // 'l' -> /login or /logout
                            sub_cnt     <= 4'd0;
                            login_is_out <= 1'b0;
                            state       <= S_URL_LOGIN;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL_STATUS: Match "tatus" after "/s"
            // --------------------------------------------------------
            S_URL_STATUS: begin
                if (rx_valid) begin
                    case (sub_cnt)
                    4'd0: begin
                        if (rx_data == 8'h74) sub_cnt <= 4'd1; // 't'
                        else state <= S_ERROR;
                    end
                    4'd1: begin
                        if (rx_data == 8'h61) sub_cnt <= 4'd2; // 'a'
                        else state <= S_ERROR;
                    end
                    4'd2: begin
                        if (rx_data == 8'h74) sub_cnt <= 4'd3; // 't'
                        else state <= S_ERROR;
                    end
                    4'd3: begin
                        if (rx_data == 8'h75) sub_cnt <= 4'd4; // 'u'
                        else state <= S_ERROR;
                    end
                    4'd4: begin
                        if (rx_data == 8'h73) sub_cnt <= 4'd5; // 's'
                        else state <= S_ERROR;
                    end
                    4'd5: begin
                        if (rx_data == CH_SP) begin
                            is_status  <= 1'b1;
                            hdr_saw_cr <= 1'b0;
                            state      <= S_SKIP_VER;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL_PACK: Parse decimal pack number
            // --------------------------------------------------------
            S_URL_PACK: begin
                if (rx_valid) begin
                    if (is_digit) begin
                        dec_accum <= dec_next;
                    end else if (rx_data == CH_SLASH) begin
                        cmd_pack  <= dec_accum[9:0];
                        has_pack  <= 1'b1;
                        dec_accum <= 22'd0;
                        state     <= S_URL_CARD;
                    end else if (rx_data == CH_QMARK) begin
                        cmd_pack  <= dec_accum[9:0];
                        has_pack  <= 1'b1;
                        dec_accum <= 22'd0;
                        qkey_shift <= 40'd0;
                        qkey_cnt   <= 3'd0;
                        state      <= S_QUERY_KEY;
                    end else if (rx_data == CH_SP) begin
                        cmd_pack   <= dec_accum[9:0];
                        has_pack   <= 1'b1;
                        hdr_saw_cr <= 1'b0;
                        state      <= S_SKIP_VER;
                    end else
                        state <= S_ERROR;
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL_CARD: Parse decimal card number
            // --------------------------------------------------------
            S_URL_CARD: begin
                if (rx_valid) begin
                    if (is_digit) begin
                        dec_accum <= dec_next;
                    end else if (rx_data == CH_SLASH) begin
                        cmd_card  <= dec_accum;
                        has_card  <= 1'b1;
                        sub_cnt   <= 4'd0;
                        state     <= S_URL_SUB;
                    end else if (rx_data == CH_QMARK) begin
                        cmd_card  <= dec_accum;
                        has_card  <= 1'b1;
                        dec_accum <= 22'd0;
                        qkey_shift <= 40'd0;
                        qkey_cnt   <= 3'd0;
                        state      <= S_QUERY_KEY;
                    end else if (rx_data == CH_SP) begin
                        cmd_card   <= dec_accum;
                        has_card   <= 1'b1;
                        hdr_saw_cr <= 1'b0;
                        state      <= S_SKIP_VER;
                    end else
                        state <= S_ERROR;
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL_SUB: Match "/_mget" or "/_pass" after card
            // --------------------------------------------------------
            S_URL_SUB: begin
                if (rx_valid) begin
                    case (sub_cnt)
                    4'd0: begin // expect '_'
                        if (rx_data == CH_UNDER)
                            sub_cnt <= 4'd1;
                        else
                            state <= S_ERROR;
                    end
                    4'd1: begin // 'm' for mget, 'p' for pass
                        if (rx_data == 8'h6D) begin      // 'm'
                            sub_is_m <= 1'b1;
                            sub_cnt  <= 4'd2;
                        end else if (rx_data == 8'h70) begin // 'p'
                            sub_is_m <= 1'b0;
                            sub_cnt  <= 4'd2;
                        end else
                            state <= S_ERROR;
                    end
                    4'd2: begin // mget: 'g', pass: 'a'
                        if (sub_is_m && rx_data == 8'h67)       sub_cnt <= 4'd3; // 'g'
                        else if (!sub_is_m && rx_data == 8'h61) sub_cnt <= 4'd3; // 'a'
                        else state <= S_ERROR;
                    end
                    4'd3: begin // mget: 'e', pass: 's'
                        if (sub_is_m && rx_data == 8'h65)       sub_cnt <= 4'd4; // 'e'
                        else if (!sub_is_m && rx_data == 8'h73) sub_cnt <= 4'd4; // 's'
                        else state <= S_ERROR;
                    end
                    4'd4: begin // mget: 't', pass: 's'
                        if (sub_is_m && rx_data == 8'h74) begin       // 't'
                            is_mget <= 1'b1;
                            sub_cnt <= 4'd5;
                        end else if (!sub_is_m && rx_data == 8'h73) begin // 's'
                            is_passwd <= 1'b1;
                            sub_cnt   <= 4'd5;
                        end else
                            state <= S_ERROR;
                    end
                    4'd5: begin // expect space
                        if (rx_data == CH_SP) begin
                            hdr_saw_cr <= 1'b0;
                            state      <= S_SKIP_VER;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_URL_LOGIN: Match "ogin" or "ogout" after "/l"
            // --------------------------------------------------------
            S_URL_LOGIN: begin
                if (rx_valid) begin
                    case (sub_cnt)
                    4'd0: begin // expect 'o'
                        if (rx_data == 8'h6F) sub_cnt <= 4'd1;
                        else state <= S_ERROR;
                    end
                    4'd1: begin // expect 'g'
                        if (rx_data == 8'h67) sub_cnt <= 4'd2;
                        else state <= S_ERROR;
                    end
                    4'd2: begin // 'i' for login, 'o' for logout
                        if (rx_data == 8'h69) begin        // 'i'
                            login_is_out <= 1'b0;
                            sub_cnt      <= 4'd3;
                        end else if (rx_data == 8'h6F) begin // 'o'
                            login_is_out <= 1'b1;
                            sub_cnt      <= 4'd3;
                        end else
                            state <= S_ERROR;
                    end
                    4'd3: begin // login: 'n', logout: 'u'
                        if (!login_is_out && rx_data == 8'h6E) begin // 'n'
                            is_login <= 1'b1;
                            sub_cnt  <= 4'd4;
                        end else if (login_is_out && rx_data == 8'h75) begin // 'u'
                            sub_cnt <= 4'd4;
                        end else
                            state <= S_ERROR;
                    end
                    4'd4: begin
                        if (!login_is_out) begin // login: expect space
                            if (rx_data == CH_SP) begin
                                hdr_saw_cr <= 1'b0;
                                state      <= S_SKIP_VER;
                            end else
                                state <= S_ERROR;
                        end else begin // logout: expect 't'
                            if (rx_data == 8'h74) begin // 't'
                                is_logout <= 1'b1;
                                sub_cnt   <= 4'd5;
                            end else
                                state <= S_ERROR;
                        end
                    end
                    4'd5: begin // logout: expect space
                        if (rx_data == CH_SP) begin
                            hdr_saw_cr <= 1'b0;
                            state      <= S_SKIP_VER;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_QUERY_KEY: Accumulate query key bytes until '='
            // --------------------------------------------------------
            S_QUERY_KEY: begin
                if (rx_valid) begin
                    if (rx_data == CH_EQ) begin
                        // Check key against "start" and "limit"
                        // "start" = {73,74,61,72,74}, "limit" = {6C,69,6D,69,74}
                        dec_accum <= 22'd0;
                        state     <= S_QUERY_VAL;
                    end else if (rx_data == CH_SP) begin
                        hdr_saw_cr <= 1'b0;
                        state      <= S_SKIP_VER;
                    end else begin
                        qkey_shift <= {qkey_shift[31:0], rx_data};
                        qkey_cnt   <= (qkey_cnt < 3'd5) ? qkey_cnt + 3'd1 : qkey_cnt;
                    end
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_QUERY_VAL: Parse decimal value for query param
            // --------------------------------------------------------
            S_QUERY_VAL: begin
                if (rx_valid) begin
                    if (is_digit) begin
                        dec_accum <= dec_next;
                    end else begin
                        // Store value based on key
                        if (qkey_cnt == 3'd5 &&
                            qkey_shift == {8'h73, 8'h74, 8'h61, 8'h72, 8'h74}) // "start"
                            list_start <= dec_accum;
                        else if (qkey_cnt == 3'd5 &&
                                 qkey_shift == {8'h6C, 8'h69, 8'h6D, 8'h69, 8'h74}) // "limit"
                            list_limit <= dec_accum[15:0];

                        if (rx_data == CH_AMP) begin
                            qkey_shift <= 40'd0;
                            qkey_cnt   <= 3'd0;
                            dec_accum  <= 22'd0;
                            state      <= S_QUERY_KEY;
                        end else if (rx_data == CH_SP) begin
                            hdr_saw_cr <= 1'b0;
                            state      <= S_SKIP_VER;
                        end else
                            state <= S_ERROR;
                    end
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_SKIP_VER: Skip HTTP version line until \r\n
            // --------------------------------------------------------
            S_SKIP_VER: begin
                if (rx_valid) begin
                    if (rx_data == CH_CR)
                        hdr_saw_cr <= 1'b1;
                    else if (rx_data == CH_LF && hdr_saw_cr) begin
                        hdr_saw_cr <= 1'b0;
                        hdr_pos    <= 3'd0;
                        state      <= S_HEADERS;
                    end else
                        hdr_saw_cr <= 1'b0;
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_HEADERS: Detect header type from first bytes
            // --------------------------------------------------------
            S_HEADERS: begin
                if (rx_valid) begin
                    case (hdr_pos)
                    3'd0: begin
                        if (rx_data == CH_CR)
                            hdr_pos <= 3'd7; // blank line detection
                        else begin
                            hdr_byte0 <= rx_data;
                            hdr_pos   <= 3'd1;
                        end
                    end
                    3'd1: begin
                        // hdr_byte0 has first char, rx_data is second
                        if (hdr_byte0 == 8'h41 && rx_data == 8'h75) begin // 'A','u'
                            auth_skip_cnt <= 3'd0;
                            state         <= S_HDR_AUTH;
                        end else if (hdr_byte0 == 8'h43 && rx_data == 8'h6F) begin // 'C','o'
                            hdr_pos <= 3'd2;
                        end else begin
                            hdr_saw_cr <= 1'b0;
                            state      <= S_HDR_SKIP;
                        end
                    end
                    3'd2: begin // Third byte for "Co..." headers
                        if (rx_data == 8'h6F) begin // 'o' → Cookie
                            cookie_match_pos <= 2'd0;
                            state            <= S_HDR_COO;
                        end else if (rx_data == 8'h6E) begin // 'n' → Content-Length
                            cl_skip_phase <= 2'd0;
                            state         <= S_HDR_CL;
                        end else begin
                            hdr_saw_cr <= 1'b0;
                            state      <= S_HDR_SKIP;
                        end
                    end
                    3'd7: begin // Expect LF for blank line (\r\n\r\n)
                        if (rx_data == CH_LF) begin
                            if (content_length > 16'd0) begin
                                body_remain <= content_length;
                                state       <= S_BODY;
                            end else
                                state <= S_DONE;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done) begin
                    // Connection closed during headers — treat as end
                    state <= S_DONE;
                end
            end

            // --------------------------------------------------------
            // S_HDR_SKIP: Skip to end of header line (\r\n)
            // --------------------------------------------------------
            S_HDR_SKIP: begin
                if (rx_valid) begin
                    if (rx_data == CH_CR)
                        hdr_saw_cr <= 1'b1;
                    else if (rx_data == CH_LF && hdr_saw_cr) begin
                        hdr_saw_cr <= 1'b0;
                        hdr_pos    <= 3'd0;
                        state      <= S_HEADERS;
                    end else
                        hdr_saw_cr <= 1'b0;
                end else if (rx_done)
                    state <= S_DONE;
            end

            // --------------------------------------------------------
            // S_HDR_AUTH: Skip to base64 value in Authorization header
            //   Scan for 'B', then match "asic " → S_BASE64
            // --------------------------------------------------------
            S_HDR_AUTH: begin
                if (rx_valid) begin
                    case (auth_skip_cnt)
                    3'd0: begin // Scan for 'B'
                        if (rx_data == 8'h42) // 'B'
                            auth_skip_cnt <= 3'd1;
                        else if (rx_data == CH_CR) begin
                            hdr_saw_cr <= 1'b1;
                            state      <= S_HDR_SKIP;
                        end
                    end
                    3'd1: begin // expect 'a'
                        if (rx_data == 8'h61) auth_skip_cnt <= 3'd2;
                        else state <= S_ERROR;
                    end
                    3'd2: begin // expect 's'
                        if (rx_data == 8'h73) auth_skip_cnt <= 3'd3;
                        else state <= S_ERROR;
                    end
                    3'd3: begin // expect 'i'
                        if (rx_data == 8'h69) auth_skip_cnt <= 3'd4;
                        else state <= S_ERROR;
                    end
                    3'd4: begin // expect 'c'
                        if (rx_data == 8'h63) auth_skip_cnt <= 3'd5;
                        else state <= S_ERROR;
                    end
                    3'd5: begin // expect ' '
                        if (rx_data == CH_SP) begin
                            b64_phase   <= 2'd0;
                            b64_buf     <= 24'd0;
                            b64_p2_pad  <= 1'b0;
                            b64_p3_pad  <= 1'b0;
                            state       <= S_BASE64;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_ERROR;
            end

            // --------------------------------------------------------
            // S_BASE64: Decode base64 characters (4 chars → 3 bytes)
            // --------------------------------------------------------
            S_BASE64: begin
                if (rx_valid) begin
                    if (rx_data == CH_CR) begin
                        // End of base64 — finalize auth lengths
                        has_basic_auth <= 1'b1;
                        auth_user_len  <= b64_user_cnt;
                        auth_pass_len  <= b64_pass_cnt;
                        hdr_saw_cr     <= 1'b1;
                        state          <= S_HDR_SKIP;
                    end else if (b64_is_valid) begin
                        case (b64_phase)
                        2'd0: begin
                            b64_buf[23:18] <= b64_is_pad ? 6'd0 : b64_lookup;
                            b64_p2_pad     <= 1'b0;
                            b64_p3_pad     <= 1'b0;
                            b64_phase      <= 2'd1;
                        end
                        2'd1: begin
                            b64_buf[17:12] <= b64_is_pad ? 6'd0 : b64_lookup;
                            b64_phase      <= 2'd2;
                        end
                        2'd2: begin
                            b64_buf[11:6]  <= b64_is_pad ? 6'd0 : b64_lookup;
                            b64_p2_pad     <= b64_is_pad;
                            b64_phase      <= 2'd3;
                        end
                        2'd3: begin
                            // Complete the quad — repack 4×6-bit into 3×8-bit
                            // {ph0[5:0], ph1[5:0], ph2[5:0], ph3[5:0]} → 3 bytes
                            b64_p3_pad <= b64_is_pad;
                            b64_out0 <= {b64_buf[23:18], b64_buf[17:16]};          // {ph0, ph1[5:4]}
                            b64_out1 <= {b64_buf[15:12], b64_buf[11:8]};           // {ph1[3:0], ph2[5:2]}
                            b64_out2 <= {b64_buf[7:6], (b64_is_pad ? 6'd0 : b64_lookup)}; // {ph2[1:0], ph3}

                            // Determine number of valid output bytes
                            if (b64_p2_pad) // phase 2 was pad → only 1 valid byte
                                b64_num_valid <= 2'd1;
                            else if (b64_is_pad) // phase 3 is pad → 2 valid bytes
                                b64_num_valid <= 2'd2;
                            else
                                b64_num_valid <= 2'd3;

                            b64_emit_idx <= 2'd0;
                            b64_phase    <= 2'd0;
                            state        <= S_B64_EMIT;
                        end
                        endcase
                    end else
                        state <= S_ERROR; // invalid base64 char
                end else if (rx_done) begin
                    // Stream ended during base64 — finalize what we have
                    has_basic_auth <= 1'b1;
                    auth_user_len  <= b64_user_cnt;
                    auth_pass_len  <= b64_pass_cnt;
                    state          <= S_DONE;
                end
            end

            // --------------------------------------------------------
            // S_B64_EMIT: Emit decoded base64 bytes (no rx_valid needed)
            // --------------------------------------------------------
            S_B64_EMIT: begin
                if (b64_emit_idx < b64_num_valid) begin
                    if (b64_emit_byte == CH_COLON) begin
                        // ':' separator — switch from user to pass
                        b64_saw_colon <= 1'b1;
                    end else if (!b64_saw_colon) begin
                        // Write to user register
                        if (b64_user_cnt < 5'd31)
                            auth_user_packed <= auth_user_packed |
                                ({248'd0, b64_emit_byte} << {b64_user_cnt, 3'b000});
                        b64_user_cnt <= b64_user_cnt + 5'd1;
                    end else begin
                        // Write to pass register
                        if (b64_pass_cnt < 5'd31)
                            auth_pass_packed <= auth_pass_packed |
                                ({248'd0, b64_emit_byte} << {b64_pass_cnt, 3'b000});
                        b64_pass_cnt <= b64_pass_cnt + 5'd1;
                    end
                    b64_emit_idx <= b64_emit_idx + 2'd1;
                end else begin
                    // All bytes emitted — return to base64 input
                    state <= S_BASE64;
                end
            end

            // --------------------------------------------------------
            // S_HDR_COO: Cookie header — skip to "sid=" then S_HEX
            // --------------------------------------------------------
            S_HDR_COO: begin
                if (rx_valid) begin
                    if (rx_data == CH_CR) begin
                        hdr_saw_cr <= 1'b1;
                        state      <= S_HDR_SKIP;
                    end else begin
                        case (cookie_match_pos)
                        2'd0: begin // scan for 's'
                            if (rx_data == 8'h73) // 's'
                                cookie_match_pos <= 2'd1;
                        end
                        2'd1: begin // expect 'i'
                            if (rx_data == 8'h69) // 'i'
                                cookie_match_pos <= 2'd2;
                            else if (rx_data == 8'h73) // another 's'
                                cookie_match_pos <= 2'd1;
                            else
                                cookie_match_pos <= 2'd0;
                        end
                        2'd2: begin // expect 'd'
                            if (rx_data == 8'h64) // 'd'
                                cookie_match_pos <= 2'd3;
                            else if (rx_data == 8'h73)
                                cookie_match_pos <= 2'd1;
                            else
                                cookie_match_pos <= 2'd0;
                        end
                        2'd3: begin // expect '='
                            if (rx_data == CH_EQ) begin
                                hex_cnt <= 6'd0;
                                state   <= S_HEX;
                            end else if (rx_data == 8'h73)
                                cookie_match_pos <= 2'd1;
                            else
                                cookie_match_pos <= 2'd0;
                        end
                        endcase
                    end
                end else if (rx_done)
                    state <= S_DONE;
            end

            // --------------------------------------------------------
            // S_HEX: Read 64 hex characters → 32-byte cookie token
            // --------------------------------------------------------
            S_HEX: begin
                if (rx_valid) begin
                    if (hex_valid_ch) begin
                        if (hex_cnt[0] == 1'b0) begin
                            // Even position: store high nibble
                            hex_hi_nib <= hex_nibble;
                        end else begin
                            // Odd position: combine and write byte
                            cookie_token_packed <= cookie_token_packed |
                                ({248'd0, hex_hi_nib, hex_nibble} << {hex_cnt[5:1], 3'b000});
                        end

                        if (hex_cnt == 6'd63) begin
                            has_cookie <= 1'b1;
                            hdr_saw_cr <= 1'b0;
                            state      <= S_HDR_SKIP;
                        end else
                            hex_cnt <= hex_cnt + 6'd1;
                    end else begin
                        // Non-hex char encountered — if CR, end of line
                        if (rx_data == CH_CR) begin
                            has_cookie <= 1'b1;
                            hdr_saw_cr <= 1'b1;
                            state      <= S_HDR_SKIP;
                        end else
                            state <= S_ERROR;
                    end
                end else if (rx_done) begin
                    has_cookie <= 1'b1;
                    state      <= S_DONE;
                end
            end

            // --------------------------------------------------------
            // S_HDR_CL: Content-Length header — skip to value
            //   We already consumed "Con". Scan for ':', skip SP.
            // --------------------------------------------------------
            S_HDR_CL: begin
                if (rx_valid) begin
                    case (cl_skip_phase)
                    2'd0: begin // scan for ':'
                        if (rx_data == CH_COLON)
                            cl_skip_phase <= 2'd1;
                        else if (rx_data == CH_CR) begin
                            hdr_saw_cr <= 1'b1;
                            state      <= S_HDR_SKIP;
                        end
                    end
                    2'd1: begin // skip spaces after ':'
                        if (rx_data == CH_SP)
                            ; // stay in phase 1
                        else if (is_digit) begin
                            cl_accum      <= {12'd0, rx_data[3:0]};
                            cl_skip_phase <= 2'd2;
                            state         <= S_HDR_CL_VAL;
                        end else
                            state <= S_ERROR;
                    end
                    default: state <= S_ERROR;
                    endcase
                end else if (rx_done)
                    state <= S_DONE;
            end

            // --------------------------------------------------------
            // S_HDR_CL_VAL: Parse Content-Length decimal value
            // --------------------------------------------------------
            S_HDR_CL_VAL: begin
                if (rx_valid) begin
                    if (is_digit) begin
                        cl_accum <= cl_next;
                    end else if (rx_data == CH_CR) begin
                        content_length <= cl_accum;
                        hdr_saw_cr     <= 1'b1;
                        state          <= S_HDR_SKIP;
                    end else
                        state <= S_ERROR;
                end else if (rx_done) begin
                    content_length <= cl_accum;
                    state          <= S_DONE;
                end
            end

            // --------------------------------------------------------
            // S_BODY: Pass through body bytes, counting down
            // --------------------------------------------------------
            S_BODY: begin
                if (rx_valid) begin
                    body_data  <= rx_data;
                    body_valid <= 1'b1;
                    body_remain <= body_remain - 16'd1;
                    if (body_remain == 16'd1) begin
                        body_done <= 1'b1;
                        state     <= S_DONE;
                    end
                end else if (rx_done) begin
                    body_done <= 1'b1;
                    state     <= S_DONE;
                end
            end

            // --------------------------------------------------------
            // S_DONE: Compute cmd_op, assert cmd_valid, return to idle
            // --------------------------------------------------------
            S_DONE: begin
                cmd_valid     <= 1'b1;
                rx_consumed   <= 1'b1;
                cmd_is_ui     <= is_ui;
                cmd_is_status <= is_status;

                // Determine cmd_op from method + URL flags
                if (is_ui || is_status) begin
                    cmd_op <= OP_READ; // UI/status pages use READ as placeholder
                end else if (method == M_GET) begin
                    if (has_card)
                        cmd_op <= OP_READ;
                    else if (has_pack)
                        cmd_op <= OP_LIST;
                    else
                        cmd_op <= OP_READ;
                end else if (method == M_PUT) begin
                    cmd_op <= OP_WRITE;
                end else if (method == M_DEL) begin
                    cmd_op <= OP_DELETE;
                end else if (method == M_POST) begin
                    if (is_mget)
                        cmd_op <= OP_MGET;
                    else if (is_login)
                        cmd_op <= OP_LOGIN;
                    else if (is_logout)
                        cmd_op <= OP_LOGOUT;
                    else if (is_passwd)
                        cmd_op <= OP_PASSWD;
                    else
                        cmd_op <= OP_WRITE; // fallback for POST without sub-path
                end else begin
                    cmd_op <= OP_READ;
                end

                busy  <= 1'b0;
                state <= S_IDLE;
            end

            // --------------------------------------------------------
            // S_ERROR: Assert parse_error, consume, return to idle
            // --------------------------------------------------------
            S_ERROR: begin
                parse_error <= 1'b1;
                rx_consumed <= 1'b1;
                busy        <= 1'b0;
                state       <= S_IDLE;
            end

            default: state <= S_ERROR;

            endcase
        end
    end

endmodule
