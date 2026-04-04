`timescale 1ns/1ps
module http_parser_tb;

    reg        clk;
    reg        rst_n;
    reg  [7:0] rx_data;
    reg        rx_valid;
    reg        rx_done;

    wire       rx_consumed;
    wire       cmd_valid;
    wire [2:0] cmd_op;
    wire [9:0] cmd_pack;
    wire [21:0] cmd_card;
    wire       cmd_is_ui;
    wire       cmd_is_status;
    wire [21:0] list_start;
    wire [15:0] list_limit;
    wire [15:0] content_length;
    wire       has_basic_auth;
    wire       has_cookie;
    wire [7:0] body_data;
    wire       body_valid;
    wire       body_done;
    wire       parse_error;
    wire       busy;
    wire [255:0] auth_user_packed;
    wire [4:0]   auth_user_len;
    wire [255:0] auth_pass_packed;
    wire [4:0]   auth_pass_len;
    wire [255:0] cookie_token_packed;

    http_parser uut (
        .clk(clk), .rst_n(rst_n),
        .rx_data(rx_data), .rx_valid(rx_valid), .rx_done(rx_done),
        .rx_consumed(rx_consumed), .cmd_valid(cmd_valid), .cmd_op(cmd_op),
        .cmd_pack(cmd_pack), .cmd_card(cmd_card),
        .cmd_is_ui(cmd_is_ui), .cmd_is_status(cmd_is_status),
        .list_start(list_start), .list_limit(list_limit),
        .content_length(content_length),
        .has_basic_auth(has_basic_auth), .has_cookie(has_cookie),
        .body_data(body_data), .body_valid(body_valid), .body_done(body_done),
        .parse_error(parse_error), .busy(busy),
        .auth_user_packed(auth_user_packed), .auth_user_len(auth_user_len),
        .auth_pass_packed(auth_pass_packed), .auth_pass_len(auth_pass_len),
        .cookie_token_packed(cookie_token_packed)
    );

    // Clock: 10ns period
    initial clk = 0;
    always #5 clk = ~clk;

    integer i;
    integer pass_count;
    integer fail_count;

    // Task: send a string byte-by-byte
    task send_string;
        input [8*256-1:0] str;
        input integer len;
        integer idx;
        begin
            for (idx = 0; idx < len; idx = idx + 1) begin
                @(posedge clk);
                rx_data  <= str[(len - 1 - idx)*8 +: 8];
                rx_valid <= 1'b1;
                @(posedge clk);
                rx_valid <= 1'b0;
                // Wait a cycle between bytes for simplicity
                @(posedge clk);
            end
        end
    endtask

    // Task: send a byte
    task send_byte;
        input [7:0] b;
        begin
            @(posedge clk);
            rx_data  <= b;
            rx_valid <= 1'b1;
            @(posedge clk);
            rx_valid <= 1'b0;
            @(posedge clk);
        end
    endtask

    // Wait for cmd_valid or parse_error or timeout
    task wait_result;
        input integer timeout;
        integer cnt;
        begin
            cnt = 0;
            while (!cmd_valid && !parse_error && cnt < timeout) begin
                @(posedge clk);
                cnt = cnt + 1;
            end
        end
    endtask

    initial begin
        $dumpfile("http_parser_tb.vcd");
        $dumpvars(0, http_parser_tb);

        rst_n    = 0;
        rx_data  = 8'd0;
        rx_valid = 0;
        rx_done  = 0;
        pass_count = 0;
        fail_count = 0;

        // Reset
        #20;
        rst_n = 1;
        #20;

        // ====================================================
        // Test 1: GET / (UI page)
        // "GET / HTTP/1.1\r\n\r\n"
        // ====================================================
        $display("--- Test 1: GET / ---");
        send_string("GET / HTTP/1.1\r\n\r\n", 18);
        wait_result(100);

        if (cmd_valid && cmd_is_ui && !parse_error) begin
            $display("  PASS: cmd_valid=%b, cmd_is_ui=%b", cmd_valid, cmd_is_ui);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_is_ui=%b, parse_error=%b", cmd_valid, cmd_is_ui, parse_error);
            fail_count = fail_count + 1;
        end

        // Wait for IDLE
        #40;

        // ====================================================
        // Test 2: GET /status
        // ====================================================
        $display("--- Test 2: GET /status ---");
        send_string("GET /status HTTP/1.1\r\n\r\n", 24);
        wait_result(200);

        if (cmd_valid && cmd_is_status && !parse_error) begin
            $display("  PASS: cmd_is_status=%b", cmd_is_status);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_is_status=%b, parse_error=%b", cmd_valid, cmd_is_status, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 3: GET /5 (LIST pack 5)
        // ====================================================
        $display("--- Test 3: GET /5 (LIST) ---");
        send_string("GET /5 HTTP/1.1\r\n\r\n", 20);
        wait_result(200);

        if (cmd_valid && cmd_op == 3'd3 && cmd_pack == 10'd5 && !parse_error) begin
            $display("  PASS: cmd_op=LIST, cmd_pack=%d", cmd_pack);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, cmd_pack=%d, parse_error=%b",
                     cmd_valid, cmd_op, cmd_pack, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 4: GET /3/42 (READ card 42 in pack 3)
        // ====================================================
        $display("--- Test 4: GET /3/42 (READ) ---");
        send_string("GET /3/42 HTTP/1.1\r\n\r\n", 22);
        wait_result(200);

        if (cmd_valid && cmd_op == 3'd0 && cmd_pack == 10'd3 && cmd_card == 22'd42 && !parse_error) begin
            $display("  PASS: cmd_op=READ, pack=%d, card=%d", cmd_pack, cmd_card);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, pack=%d, card=%d, parse_error=%b",
                     cmd_valid, cmd_op, cmd_pack, cmd_card, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 5: DELETE /7/100
        // ====================================================
        $display("--- Test 5: DELETE /7/100 ---");
        send_string("DELETE /7/100 HTTP/1.1\r\n\r\n", 26);
        wait_result(200);

        if (cmd_valid && cmd_op == 3'd2 && cmd_pack == 10'd7 && cmd_card == 22'd100 && !parse_error) begin
            $display("  PASS: cmd_op=DELETE, pack=%d, card=%d", cmd_pack, cmd_card);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, pack=%d, card=%d, parse_error=%b",
                     cmd_valid, cmd_op, cmd_pack, cmd_card, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 6: PUT /2/10 with Content-Length and body
        // ====================================================
        $display("--- Test 6: PUT /2/10 with body ---");
        send_string("PUT /2/10 HTTP/1.1\r\nContent-Length: 3\r\n\r\n", 42);
        // Wait for body state
        #20;
        // Send 3 body bytes
        send_byte(8'hAA);
        send_byte(8'hBB);
        send_byte(8'hCC);
        wait_result(200);

        if (cmd_valid && cmd_op == 3'd1 && cmd_pack == 10'd2 && cmd_card == 22'd10 &&
            content_length == 16'd3 && !parse_error) begin
            $display("  PASS: cmd_op=WRITE, pack=%d, card=%d, CL=%d", cmd_pack, cmd_card, content_length);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, pack=%d, card=%d, CL=%d, parse_error=%b",
                     cmd_valid, cmd_op, cmd_pack, cmd_card, content_length, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 7: GET /10?start=5&limit=50
        // ====================================================
        $display("--- Test 7: GET /10?start=5&limit=50 ---");
        send_string("GET /10?start=5&limit=50 HTTP/1.1\r\n\r\n", 38);
        wait_result(200);

        if (cmd_valid && cmd_op == 3'd3 && cmd_pack == 10'd10 &&
            list_start == 22'd5 && list_limit == 16'd50 && !parse_error) begin
            $display("  PASS: LIST pack=%d, start=%d, limit=%d", cmd_pack, list_start, list_limit);
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, pack=%d, start=%d, limit=%d, parse_error=%b",
                     cmd_valid, cmd_op, cmd_pack, list_start, list_limit, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Test 8: POST /login with Authorization: Basic header
        // Base64 of "admin:secret" = "YWRtaW46c2VjcmV0"
        // ====================================================
        $display("--- Test 8: POST /login with Basic auth ---");
        send_string("POST /login HTTP/1.1\r\nAuthorization: Basic YWRtaW46c2VjcmV0\r\n\r\n", 64);
        wait_result(400);

        if (cmd_valid && cmd_op == 3'd5 && has_basic_auth && !parse_error) begin
            $display("  PASS: cmd_op=LOGIN, has_basic_auth=%b", has_basic_auth);
            $display("    user_len=%d, pass_len=%d", auth_user_len, auth_pass_len);
            // Check user = "admin" (5 bytes: 61 64 6D 69 6E)
            if (auth_user_len == 5'd5 &&
                auth_user_packed[7:0]   == 8'h61 && // 'a'
                auth_user_packed[15:8]  == 8'h64 && // 'd'
                auth_user_packed[23:16] == 8'h6D && // 'm'
                auth_user_packed[31:24] == 8'h69 && // 'i'
                auth_user_packed[39:32] == 8'h6E)   // 'n'
            begin
                $display("    user='admin' CORRECT");
            end else begin
                $display("    user bytes: %h %h %h %h %h (len=%d)",
                    auth_user_packed[7:0], auth_user_packed[15:8],
                    auth_user_packed[23:16], auth_user_packed[31:24],
                    auth_user_packed[39:32], auth_user_len);
            end
            // Check pass = "secret" (6 bytes: 73 65 63 72 65 74)
            if (auth_pass_len == 5'd6 &&
                auth_pass_packed[7:0]   == 8'h73 && // 's'
                auth_pass_packed[15:8]  == 8'h65 && // 'e'
                auth_pass_packed[23:16] == 8'h63 && // 'c'
                auth_pass_packed[31:24] == 8'h72 && // 'r'
                auth_pass_packed[39:32] == 8'h65 && // 'e'
                auth_pass_packed[47:40] == 8'h74)   // 't'
            begin
                $display("    pass='secret' CORRECT");
            end else begin
                $display("    pass bytes: %h %h %h %h %h %h (len=%d)",
                    auth_pass_packed[7:0], auth_pass_packed[15:8],
                    auth_pass_packed[23:16], auth_pass_packed[31:24],
                    auth_pass_packed[39:32], auth_pass_packed[47:40], auth_pass_len);
            end
            pass_count = pass_count + 1;
        end else begin
            $display("  FAIL: cmd_valid=%b, cmd_op=%d, has_basic_auth=%b, parse_error=%b",
                     cmd_valid, cmd_op, has_basic_auth, parse_error);
            fail_count = fail_count + 1;
        end

        #40;

        // ====================================================
        // Summary
        // ====================================================
        $display("");
        $display("========================================");
        $display("  Results: %0d PASS, %0d FAIL", pass_count, fail_count);
        $display("========================================");

        #100;
        $finish;
    end

endmodule
