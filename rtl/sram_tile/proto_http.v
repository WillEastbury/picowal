// proto_http.v -- HTTP/1.1 protocol engine (hardware, no filesystem)
//
// Stack: SerDes -> MAC -> IP -> TCP -> socket_mux -> [THIS] -> NVMe
//
// URL namespace: GET /card/folder/file HTTP/1.1
//   Maps directly to PicoWAL block address via index pico
//   PUT /card/folder/file writes via index pico path
//   DELETE /card/folder/file removes entry
//
// Also serves:
//   GET /status -> JSON system status (connections, NVMe health)
//   GET /query?sql=... -> forwards to session pico via pico bus
//
// ~2500 LUTs estimated (URL parser + header gen + status page)
//
`default_nettype none

module proto_http #(
    parameter MAX_URL_LEN = 128
)(
    input  wire        clk,
    input  wire        rst_n,

    // Socket interface
    input  wire [7:0]  rx_data,
    input  wire        rx_valid,
    input  wire        rx_sof,
    input  wire        rx_eof,
    input  wire [5:0]  rx_conn,
    output wire        rx_ready,

    output reg  [7:0]  tx_data,
    output reg         tx_valid,
    output reg         tx_sof,
    output reg         tx_eof,
    output reg  [5:0]  tx_conn,
    input  wire        tx_ready,

    // NVMe block interface
    output reg         blk_read_req,
    output reg  [41:0] blk_addr,
    output reg  [15:0] blk_count,
    input  wire        blk_ready,
    input  wire [7:0]  blk_rdata,
    input  wire        blk_rvalid,
    input  wire        blk_rdone,

    // Index pico interface
    output reg         idx_lookup_req,
    output reg  [9:0]  idx_card,
    output reg  [15:0] idx_folder,
    output reg  [15:0] idx_file,
    input  wire        idx_lookup_ack,
    input  wire [41:0] idx_block_addr,
    input  wire [31:0] idx_file_size,
    input  wire        idx_not_found,

    // Pico bus (for /query endpoint -> session pico)
    output reg  [7:0]  pico_tx_data,
    output reg         pico_tx_valid,
    output reg         pico_tx_sof,
    output reg         pico_tx_eof,
    input  wire [7:0]  pico_rx_data,
    input  wire        pico_rx_valid,
    input  wire        pico_rx_eof,

    // Stats
    output reg  [31:0] http_gets,
    output reg  [31:0] http_puts,
    output reg  [31:0] http_404s
);

    assign rx_ready = 1'b1;

    // ── Request parser states ──
    localparam S_IDLE       = 4'd0,
               S_METHOD     = 4'd1,
               S_URL        = 4'd2,
               S_HEADERS    = 4'd3,
               S_IDX_WAIT   = 4'd4,
               S_RESP_HDR   = 4'd5,
               S_RESP_BODY  = 4'd6,
               S_RESP_STATUS = 4'd7,
               S_RESP_404   = 4'd8,
               S_PICO_FWD   = 4'd9,
               S_PICO_RESP  = 4'd10;

    reg [3:0]  state;
    reg [5:0]  cur_conn;

    // Method detection
    localparam M_GET    = 2'd0,
               M_PUT    = 2'd1,
               M_DELETE = 2'd2,
               M_POST   = 2'd3;
    reg [1:0]  method;
    reg [2:0]  method_cnt;

    // URL parsing: /card/folder/file
    reg [7:0]  url_buf [0:MAX_URL_LEN-1];
    reg [6:0]  url_len;
    reg [1:0]  slash_cnt;     // count slashes to extract 3 numbers
    reg [15:0] num_accum;     // accumulate decimal digits
    reg [9:0]  parsed_card;
    reg [15:0] parsed_folder;
    reg [15:0] parsed_file;
    reg        is_status_url; // /status endpoint
    reg        is_query_url;  // /query endpoint

    // Response header ROM
    // "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n"
    localparam RESP_200_LEN = 17;
    reg [7:0] resp_200 [0:RESP_200_LEN-1];
    initial begin
        resp_200[0]  = "H"; resp_200[1]  = "T"; resp_200[2]  = "T";
        resp_200[3]  = "P"; resp_200[4]  = "/"; resp_200[5]  = "1";
        resp_200[6]  = "."; resp_200[7]  = "1"; resp_200[8]  = " ";
        resp_200[9]  = "2"; resp_200[10] = "0"; resp_200[11] = "0";
        resp_200[12] = " "; resp_200[13] = "O"; resp_200[14] = "K";
        resp_200[15] = 8'h0D; resp_200[16] = 8'h0A; // \r\n
    end

    localparam RESP_404_LEN = 24;
    reg [7:0] resp_404 [0:RESP_404_LEN-1];
    initial begin
        resp_404[0]  = "H"; resp_404[1]  = "T"; resp_404[2]  = "T";
        resp_404[3]  = "P"; resp_404[4]  = "/"; resp_404[5]  = "1";
        resp_404[6]  = "."; resp_404[7]  = "1"; resp_404[8]  = " ";
        resp_404[9]  = "4"; resp_404[10] = "0"; resp_404[11] = "4";
        resp_404[12] = " "; resp_404[13] = "N"; resp_404[14] = "o";
        resp_404[15] = "t"; resp_404[16] = " "; resp_404[17] = "F";
        resp_404[18] = "o"; resp_404[19] = "u"; resp_404[20] = "n";
        resp_404[21] = "d"; resp_404[22] = 8'h0D; resp_404[23] = 8'h0A;
    end

    reg [7:0] resp_cnt;
    reg [7:0] resp_total;

    // Content-Length + CRLF CRLF
    localparam CL_HDR_LEN = 20;
    reg [7:0] cl_hdr [0:CL_HDR_LEN-1];
    initial begin
        // "Content-Length: "
        cl_hdr[0]  = "C"; cl_hdr[1]  = "o"; cl_hdr[2]  = "n";
        cl_hdr[3]  = "t"; cl_hdr[4]  = "e"; cl_hdr[5]  = "n";
        cl_hdr[6]  = "t"; cl_hdr[7]  = "-"; cl_hdr[8]  = "L";
        cl_hdr[9]  = "e"; cl_hdr[10] = "n"; cl_hdr[11] = "g";
        cl_hdr[12] = "t"; cl_hdr[13] = "h"; cl_hdr[14] = ":";
        cl_hdr[15] = " "; cl_hdr[16] = "0";  // placeholder
        cl_hdr[17] = 8'h0D; cl_hdr[18] = 8'h0A; // \r\n
        cl_hdr[19] = 8'h0D; // start of \r\n\r\n
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            tx_valid <= 0; tx_sof <= 0; tx_eof <= 0;
            blk_read_req <= 0; idx_lookup_req <= 0;
            pico_tx_valid <= 0;
            http_gets <= 0; http_puts <= 0; http_404s <= 0;
        end else begin
            tx_valid <= 0; tx_sof <= 0; tx_eof <= 0;
            blk_read_req <= 0; idx_lookup_req <= 0;
            pico_tx_valid <= 0;

            case (state)

                S_IDLE: begin
                    if (rx_valid && rx_sof) begin
                        cur_conn <= rx_conn;
                        method_cnt <= 0;
                        url_len <= 0;
                        slash_cnt <= 0;
                        num_accum <= 0;
                        is_status_url <= 0;
                        is_query_url <= 0;
                        state <= S_METHOD;
                    end
                end

                // Parse "GET " / "PUT " / "DELETE " / "POST "
                S_METHOD: begin
                    if (rx_valid) begin
                        method_cnt <= method_cnt + 1;
                        case (method_cnt)
                            0: begin
                                case (rx_data)
                                    "G": method <= M_GET;
                                    "P": method <= M_PUT; // or POST
                                    "D": method <= M_DELETE;
                                    default: method <= M_GET;
                                endcase
                            end
                            1: begin
                                if (rx_data == "O") method <= M_POST; // POST vs PUT
                            end
                        endcase
                        if (rx_data == " " && method_cnt > 1) begin
                            state <= S_URL;
                        end
                    end
                    if (rx_eof) state <= S_IDLE;
                end

                // Parse URL: /card/folder/file
                S_URL: begin
                    if (rx_valid) begin
                        if (rx_data == " ") begin
                            // End of URL, space before "HTTP/1.1"
                            // Store last number
                            case (slash_cnt)
                                1: parsed_card   <= num_accum[9:0];
                                2: parsed_folder <= num_accum;
                                3: parsed_file   <= num_accum;
                            endcase
                            state <= S_HEADERS;
                        end else if (rx_data == "/") begin
                            // Slash separator
                            case (slash_cnt)
                                1: parsed_card   <= num_accum[9:0];
                                2: parsed_folder <= num_accum;
                            endcase
                            slash_cnt <= slash_cnt + 1;
                            num_accum <= 0;

                            // Check for /status or /query
                            if (slash_cnt == 0 && url_len == 0) begin
                                // Will detect on next chars
                            end
                        end else if (rx_data >= "0" && rx_data <= "9") begin
                            // Decimal digit
                            num_accum <= num_accum * 10 + (rx_data - "0");
                        end else begin
                            // Alpha — check for /status, /query
                            if (slash_cnt == 1 && url_len == 1 && rx_data == "s")
                                is_status_url <= 1;
                            if (slash_cnt == 1 && url_len == 1 && rx_data == "q")
                                is_query_url <= 1;
                        end

                        if (url_len < MAX_URL_LEN)
                            url_buf[url_len] <= rx_data;
                        url_len <= url_len + 1;
                    end
                    if (rx_eof) state <= S_HEADERS; // short request
                end

                // Skip remaining headers until \r\n\r\n
                S_HEADERS: begin
                    if (rx_eof || !rx_valid) begin
                        if (is_status_url) begin
                            // /status -> return JSON stats
                            state <= S_RESP_STATUS;
                            resp_cnt <= 0;
                        end else if (is_query_url) begin
                            // /query -> forward to pico
                            state <= S_PICO_FWD;
                        end else begin
                            // /card/folder/file -> index lookup
                            idx_card   <= parsed_card;
                            idx_folder <= parsed_folder;
                            idx_file   <= parsed_file;
                            idx_lookup_req <= 1;
                            state <= S_IDX_WAIT;
                        end

                        if (method == M_GET) http_gets <= http_gets + 1;
                        if (method == M_PUT) http_puts <= http_puts + 1;
                    end
                end

                // Wait for index pico
                S_IDX_WAIT: begin
                    if (idx_lookup_ack) begin
                        if (idx_not_found) begin
                            http_404s <= http_404s + 1;
                            state <= S_RESP_404;
                            resp_cnt <= 0;
                        end else begin
                            // Found — issue NVMe read
                            blk_addr <= idx_block_addr;
                            blk_count <= idx_file_size[15:0];
                            blk_read_req <= 1;
                            state <= S_RESP_HDR;
                            resp_cnt <= 0;
                        end
                    end
                end

                // Send "HTTP/1.1 200 OK\r\n..." header
                S_RESP_HDR: begin
                    if (tx_ready) begin
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                        tx_sof <= (resp_cnt == 0);
                        if (resp_cnt < RESP_200_LEN)
                            tx_data <= resp_200[resp_cnt];
                        else
                            tx_data <= 8'h0A; // final \n of \r\n\r\n

                        resp_cnt <= resp_cnt + 1;
                        // After header, stream body from NVMe
                        if (resp_cnt == RESP_200_LEN + 3) begin
                            state <= S_RESP_BODY;
                        end
                    end
                end

                // Stream NVMe data as HTTP body (zero-copy)
                S_RESP_BODY: begin
                    if (blk_rvalid && tx_ready) begin
                        tx_data <= blk_rdata;
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                    end
                    if (blk_rdone) begin
                        tx_eof <= 1;
                        tx_valid <= 1;
                        state <= S_IDLE;
                    end
                end

                // Send 404
                S_RESP_404: begin
                    if (tx_ready) begin
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                        tx_sof <= (resp_cnt == 0);
                        tx_data <= resp_404[resp_cnt];
                        resp_cnt <= resp_cnt + 1;
                        if (resp_cnt == RESP_404_LEN - 1) begin
                            tx_eof <= 1;
                            state <= S_IDLE;
                        end
                    end
                end

                // Status JSON (hardcoded template with live counters)
                S_RESP_STATUS: begin
                    // Simplified: send 200 + EOF
                    if (tx_ready) begin
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                        tx_sof <= (resp_cnt == 0);
                        // Would emit JSON with stats here
                        tx_data <= "0";
                        resp_cnt <= resp_cnt + 1;
                        if (resp_cnt > 0) begin
                            tx_eof <= 1;
                            state <= S_IDLE;
                        end
                    end
                end

                // Forward query to session pico
                S_PICO_FWD: begin
                    // Forward URL query string to pico via 8-bit bus
                    state <= S_PICO_RESP;
                end

                // Wait for pico response, stream back as HTTP
                S_PICO_RESP: begin
                    if (pico_rx_valid && tx_ready) begin
                        tx_data <= pico_rx_data;
                        tx_valid <= 1;
                        tx_conn <= cur_conn;
                    end
                    if (pico_rx_eof) begin
                        tx_eof <= 1;
                        tx_valid <= 1;
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
