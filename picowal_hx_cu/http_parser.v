// http_parser.v -- Hardware HTTP request parser
// Target: iCE40HX8K (Alchitry Cu)
//
// Parses incoming HTTP/1.1 requests directly in LUTs:
//   1. Extracts method (GET/POST/PUT/DELETE)
//   2. Extracts URL path → maps to (tenant, pack, card) address
//   3. Skips headers (scans for \r\n\r\n)
//   4. Raises interrupt to assigned context with request info
//
// URL format: /{tenant}/{pack}/{card}
// Examples:
//   GET /0/users/42       → tenant=0, pack="users"(hashed), card=42
//   POST /0/users         → tenant=0, pack="users", card=NEW
//   GET /0/users          → tenant=0, pack="users", card=ALL (list)

`default_nettype none

module http_parser (
    input  wire        clk,
    input  wire        rst_n,

    // Byte stream input (from W5100S RX via SPI)
    input  wire [7:0]  rx_byte,
    input  wire        rx_valid,
    input  wire [1:0]  socket_id,       // which W5100S socket (0-3)

    // Parsed request output
    output reg  [1:0]  req_method,      // 0=GET, 1=POST, 2=PUT, 3=DELETE
    output reg  [3:0]  req_tenant,      // tenant ID (0-15)
    output reg  [7:0]  req_pack,        // pack ID (hashed from name)
    output reg  [15:0] req_card,        // card ID (from URL number)
    output reg         req_is_list,     // GET on pack (no card specified)
    output reg         req_valid,       // full request parsed, fields valid
    output reg  [1:0]  req_socket,      // which socket this came from

    // Content body (for POST/PUT)
    output reg  [8:0]  body_length,     // bytes of body following headers
    output reg         body_start,      // headers done, body begins

    // Interrupt: signal to scheduler that request is ready
    output reg         irq_request_ready,
    output reg  [2:0]  irq_target_ctx   // which context to wake
);

    // ─── HTTP methods ────────────────────────────────────────────────
    localparam METHOD_GET    = 2'd0;
    localparam METHOD_POST   = 2'd1;
    localparam METHOD_PUT    = 2'd2;
    localparam METHOD_DELETE = 2'd3;

    // ─── Parser states ───────────────────────────────────────────────
    localparam S_IDLE       = 4'd0;
    localparam S_METHOD     = 4'd1;     // reading method (GET/POST/etc)
    localparam S_URL_SLASH1 = 4'd2;     // skip first /
    localparam S_TENANT     = 4'd3;     // parse tenant number
    localparam S_PACK       = 4'd4;     // parse pack name (hash it)
    localparam S_CARD       = 4'd5;     // parse card number
    localparam S_SKIP_VER   = 4'd6;     // skip " HTTP/1.1\r\n"
    localparam S_HEADERS    = 4'd7;     // skip headers, watch for \r\n\r\n
    localparam S_BODY       = 4'd8;     // body receiving
    localparam S_DONE       = 4'd9;
    localparam S_ERROR      = 4'd10;

    reg [3:0]  state;
    reg [2:0]  method_pos;      // position within method string
    reg [7:0]  method_buf [0:3]; // first 4 chars of method
    reg [7:0]  pack_hash;       // rolling hash of pack name
    reg [15:0] card_accum;      // accumulator for card number
    reg        card_seen;       // at least one digit in card field
    reg [1:0]  crlf_count;     // counts consecutive \r\n sequences
    reg        prev_cr;         // previous byte was \r
    reg        prev_lf;         // previous byte was \n (after \r)

    // Socket-to-context mapping (round-robin: socket N → context N)
    wire [2:0] target_ctx = {1'b0, socket_id};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state             <= S_IDLE;
            req_valid         <= 1'b0;
            body_start        <= 1'b0;
            irq_request_ready <= 1'b0;
            method_pos        <= 3'd0;
            pack_hash         <= 8'd0;
            card_accum        <= 16'd0;
            card_seen         <= 1'b0;
            crlf_count        <= 2'd0;
            prev_cr           <= 1'b0;
            prev_lf           <= 1'b0;
        end else begin
            req_valid         <= 1'b0;
            body_start        <= 1'b0;
            irq_request_ready <= 1'b0;

            if (rx_valid) begin
                case (state)
                    S_IDLE: begin
                        // First byte of new request
                        method_buf[0] <= rx_byte;
                        method_pos    <= 3'd1;
                        pack_hash     <= 8'd0;
                        card_accum    <= 16'd0;
                        card_seen     <= 1'b0;
                        crlf_count    <= 2'd0;
                        req_is_list   <= 1'b0;
                        req_socket    <= socket_id;
                        state         <= S_METHOD;
                    end

                    S_METHOD: begin
                        if (rx_byte == 8'h20) begin  // space = end of method
                            // Decode method from first char
                            case (method_buf[0])
                                8'h47: req_method <= METHOD_GET;     // 'G'
                                8'h50: begin                        // 'P'
                                    if (method_pos >= 3'd2 && method_buf[1] == 8'h4F)
                                        req_method <= METHOD_POST;  // "PO"
                                    else
                                        req_method <= METHOD_PUT;   // "PU"
                                end
                                8'h44: req_method <= METHOD_DELETE;  // 'D'
                                default: state <= S_ERROR;
                            endcase
                            state <= S_URL_SLASH1;
                        end else if (method_pos < 3'd4) begin
                            method_buf[method_pos] <= rx_byte;
                            method_pos <= method_pos + 3'd1;
                        end
                    end

                    S_URL_SLASH1: begin
                        if (rx_byte == 8'h2F)       // '/'
                            state <= S_TENANT;
                        else
                            state <= S_ERROR;
                    end

                    S_TENANT: begin
                        if (rx_byte == 8'h2F) begin // '/'
                            state <= S_PACK;
                        end else if (rx_byte >= 8'h30 && rx_byte <= 8'h39) begin
                            // Digit: tenant number
                            req_tenant <= rx_byte[3:0];
                        end else begin
                            state <= S_ERROR;
                        end
                    end

                    S_PACK: begin
                        if (rx_byte == 8'h2F) begin // '/'
                            req_pack <= pack_hash;
                            state    <= S_CARD;
                        end else if (rx_byte == 8'h20 || rx_byte == 8'h3F) begin
                            // Space or '?' = end of URL (no card = list)
                            req_pack    <= pack_hash;
                            req_is_list <= 1'b1;
                            req_card    <= 16'd0;
                            state       <= S_SKIP_VER;
                        end else begin
                            // Hash pack name: simple xor-rotate
                            pack_hash <= {pack_hash[6:0], pack_hash[7]} ^ rx_byte;
                        end
                    end

                    S_CARD: begin
                        if (rx_byte == 8'h20 || rx_byte == 8'h3F) begin
                            // Space or '?' = end of URL
                            req_card <= card_accum;
                            if (!card_seen)
                                req_is_list <= 1'b1;
                            state <= S_SKIP_VER;
                        end else if (rx_byte >= 8'h30 && rx_byte <= 8'h39) begin
                            // Digit
                            card_accum <= card_accum * 10 + {12'd0, rx_byte[3:0]};
                            card_seen  <= 1'b1;
                        end else begin
                            state <= S_ERROR;
                        end
                    end

                    S_SKIP_VER: begin
                        // Skip until \n (end of request line)
                        if (rx_byte == 8'h0A) begin
                            state      <= S_HEADERS;
                            prev_cr    <= 1'b0;
                            crlf_count <= 2'd1;  // we just saw the first \r\n
                        end
                    end

                    S_HEADERS: begin
                        // Watch for \r\n\r\n (blank line = end of headers)
                        if (rx_byte == 8'h0D) begin
                            prev_cr <= 1'b1;
                        end else if (rx_byte == 8'h0A && prev_cr) begin
                            prev_cr <= 1'b0;
                            if (crlf_count >= 2'd1) begin
                                // Double \r\n = headers done
                                state <= S_DONE;
                            end else begin
                                crlf_count <= crlf_count + 2'd1;
                            end
                        end else begin
                            prev_cr    <= 1'b0;
                            crlf_count <= 2'd0;
                        end
                    end

                    S_DONE: begin
                        // Request fully parsed
                        req_valid         <= 1'b1;
                        irq_request_ready <= 1'b1;
                        irq_target_ctx    <= target_ctx;
                        state             <= S_IDLE;
                    end

                    S_ERROR: begin
                        // Bad request — skip until next connection
                        if (rx_byte == 8'h0A)  // wait for any \n then reset
                            state <= S_IDLE;
                    end

                    default: state <= S_IDLE;
                endcase
            end
        end
    end

endmodule
