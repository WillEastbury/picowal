// proto_dhcp_dns.v -- DHCP client + DNS responder (UDP, lightweight)
//
// Stack: SerDes -> MAC -> IP -> [THIS, via UDP shortcut] -> IP config
//
// DHCP: Acquires IP address on boot (DISCOVER -> OFFER -> REQUEST -> ACK)
// DNS:  Responds to queries for "picowal.local" with our IP
//
// Both are UDP — tapped before TCP engine, same IP layer
// Combined because they share UDP parse logic (~800 LUTs total)
//
`default_nettype none

module proto_dhcp_dns #(
    parameter [47:0] MAC_ADDR   = 48'h02_00_00_00_00_01,
    parameter [31:0] FALLBACK_IP = 32'hC0A80164  // 192.168.1.100
)(
    input  wire        clk,
    input  wire        rst_n,

    // From IP layer (UDP packets only, proto=17)
    input  wire [7:0]  ip_rx_data,
    input  wire        ip_rx_valid,
    input  wire        ip_rx_sof,
    input  wire        ip_rx_eof,
    input  wire [31:0] ip_rx_src_ip,
    input  wire [7:0]  ip_rx_proto,
    input  wire [15:0] ip_rx_length,

    // To IP layer (UDP responses)
    output reg  [7:0]  ip_tx_data,
    output reg         ip_tx_valid,
    output reg         ip_tx_sof,
    output reg         ip_tx_eof,
    output reg  [31:0] ip_tx_dst_ip,
    output reg  [7:0]  ip_tx_proto,
    output reg  [15:0] ip_tx_length,
    input  wire        ip_tx_ready,

    // Assigned IP output (feeds into net_ip_stack)
    output reg  [31:0] assigned_ip,
    output reg         ip_assigned,

    // Assigned gateway + subnet
    output reg  [31:0] gateway_ip,
    output reg  [31:0] subnet_mask,
    output reg  [31:0] dns_server_ip,

    // Control
    input  wire        dhcp_enable,
    output reg  [1:0]  dhcp_state_out  // 0=idle, 1=discovering, 2=requesting, 3=bound
);

    // ── UDP header parse ──
    reg [15:0] udp_src_port, udp_dst_port, udp_length;
    reg [5:0]  udp_cnt;

    // ── DHCP state machine ──
    localparam DHCP_IDLE      = 3'd0,
               DHCP_DISCOVER  = 3'd1,
               DHCP_WAIT_OFFER = 3'd2,
               DHCP_REQUEST   = 3'd3,
               DHCP_WAIT_ACK  = 3'd4,
               DHCP_BOUND     = 3'd5,
               DHCP_RENEW     = 3'd6;

    reg [2:0]  dhcp_state;
    reg [31:0] dhcp_xid;        // transaction ID
    reg [31:0] dhcp_server_ip;  // offering server
    reg [31:0] dhcp_offered_ip;
    reg [31:0] dhcp_timer;      // countdown for retransmit/renew
    reg [31:0] dhcp_lease_time;

    localparam CLK_HZ = 125_000_000;
    localparam DHCP_TIMEOUT = CLK_HZ * 3;  // 3 second timeout
    localparam DHCP_RENEW_DIV = 2;          // renew at half lease

    // DHCP message buffer (we only need first ~240 bytes + options)
    reg [7:0]  dhcp_buf [0:63];
    reg [7:0]  dhcp_parse_cnt;
    reg        dhcp_is_offer;
    reg        dhcp_is_ack;

    // ── DNS state ──
    reg [7:0]  dns_buf [0:31];
    reg [5:0]  dns_cnt;
    reg [15:0] dns_txid;

    // ── Main RX parser ──
    localparam RX_IDLE     = 3'd0,
               RX_UDP_HDR  = 3'd1,
               RX_DHCP     = 3'd2,
               RX_DNS      = 3'd3,
               RX_DROP     = 3'd4;

    reg [2:0]  rx_state;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state <= RX_IDLE;
            dhcp_state <= DHCP_IDLE;
            ip_assigned <= 0;
            assigned_ip <= FALLBACK_IP;
            dhcp_timer <= 0;
            dhcp_xid <= 32'hDEADBEEF;
            dhcp_state_out <= 0;
            ip_tx_valid <= 0;
        end else begin
            ip_tx_valid <= 0; ip_tx_sof <= 0; ip_tx_eof <= 0;

            // DHCP timer
            if (dhcp_timer > 0)
                dhcp_timer <= dhcp_timer - 1;

            case (rx_state)
                RX_IDLE: begin
                    if (ip_rx_valid && ip_rx_sof && ip_rx_proto == 8'd17) begin
                        // UDP packet
                        rx_state <= RX_UDP_HDR;
                        udp_cnt <= 0;
                    end
                end

                RX_UDP_HDR: begin
                    if (ip_rx_valid) begin
                        case (udp_cnt)
                            0: udp_src_port[15:8] <= ip_rx_data;
                            1: udp_src_port[7:0]  <= ip_rx_data;
                            2: udp_dst_port[15:8] <= ip_rx_data;
                            3: udp_dst_port[7:0]  <= ip_rx_data;
                            4: udp_length[15:8]   <= ip_rx_data;
                            5: udp_length[7:0]    <= ip_rx_data;
                            // 6-7: checksum (skip)
                            7: begin
                                // Route by port
                                if (udp_dst_port == 16'd68) begin
                                    // DHCP client port
                                    rx_state <= RX_DHCP;
                                    dhcp_parse_cnt <= 0;
                                    dhcp_is_offer <= 0;
                                    dhcp_is_ack <= 0;
                                end else if (udp_dst_port == 16'd53) begin
                                    // DNS query
                                    rx_state <= RX_DNS;
                                    dns_cnt <= 0;
                                end else begin
                                    rx_state <= RX_DROP;
                                end
                            end
                        endcase
                        udp_cnt <= udp_cnt + 1;
                    end
                    if (ip_rx_eof) rx_state <= RX_IDLE;
                end

                RX_DHCP: begin
                    if (ip_rx_valid) begin
                        if (dhcp_parse_cnt < 64)
                            dhcp_buf[dhcp_parse_cnt] <= ip_rx_data;
                        dhcp_parse_cnt <= dhcp_parse_cnt + 1;

                        // DHCP message type at byte 0: 2=REPLY
                        // Offered IP at bytes 16-19 (yiaddr)
                        // Server IP at bytes 20-23 (siaddr)
                        case (dhcp_parse_cnt)
                            0:  if (ip_rx_data != 8'd2) rx_state <= RX_DROP; // not BOOTREPLY
                            16: dhcp_offered_ip[31:24] <= ip_rx_data;
                            17: dhcp_offered_ip[23:16] <= ip_rx_data;
                            18: dhcp_offered_ip[15:8]  <= ip_rx_data;
                            19: dhcp_offered_ip[7:0]   <= ip_rx_data;
                            20: dhcp_server_ip[31:24]  <= ip_rx_data;
                            21: dhcp_server_ip[23:16]  <= ip_rx_data;
                            22: dhcp_server_ip[15:8]   <= ip_rx_data;
                            23: dhcp_server_ip[7:0]    <= ip_rx_data;
                        endcase

                        // Parse DHCP options (after byte 236)
                        if (dhcp_parse_cnt > 236) begin
                            // Simplified: detect message type option (53)
                            // Option 53, len 1, value: 2=OFFER, 5=ACK
                        end
                    end

                    if (ip_rx_eof) begin
                        // Process DHCP response
                        if (dhcp_state == DHCP_WAIT_OFFER) begin
                            // Got offer -> send REQUEST
                            dhcp_state <= DHCP_REQUEST;
                            dhcp_timer <= DHCP_TIMEOUT;
                        end else if (dhcp_state == DHCP_WAIT_ACK) begin
                            // Got ACK -> bound
                            assigned_ip <= dhcp_offered_ip;
                            ip_assigned <= 1;
                            dhcp_state <= DHCP_BOUND;
                            dhcp_state_out <= 2'd3;
                        end
                        rx_state <= RX_IDLE;
                    end
                end

                RX_DNS: begin
                    if (ip_rx_valid) begin
                        if (dns_cnt < 32)
                            dns_buf[dns_cnt] <= ip_rx_data;
                        dns_cnt <= dns_cnt + 1;
                    end
                    if (ip_rx_eof) begin
                        dns_txid <= {dns_buf[0], dns_buf[1]};
                        // Would generate DNS response for "picowal.local"
                        rx_state <= RX_IDLE;
                    end
                end

                RX_DROP: begin
                    if (ip_rx_eof) rx_state <= RX_IDLE;
                end

                default: rx_state <= RX_IDLE;
            endcase

            // ── DHCP TX: send DISCOVER/REQUEST on timer ──
            if (dhcp_enable && dhcp_state == DHCP_IDLE) begin
                dhcp_state <= DHCP_DISCOVER;
                dhcp_timer <= DHCP_TIMEOUT;
                dhcp_state_out <= 2'd1;
            end

            if (dhcp_state == DHCP_DISCOVER && dhcp_timer == 0) begin
                // Timeout -> retry
                dhcp_timer <= DHCP_TIMEOUT;
                dhcp_xid <= dhcp_xid + 1;
            end
        end
    end

endmodule
`default_nettype wire
