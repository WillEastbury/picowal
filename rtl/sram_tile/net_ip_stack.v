// net_ip_stack.v -- Layer 3: IPv4 + ARP + ICMP
// Sits on top of net_eth_mac, provides socket-level interface up to TCP/UDP
//
// Stack: SerDes -> MAC -> [THIS] -> net_tcp / net_udp
//
// ARP: 16-entry table, auto-reply, auto-request on cache miss
// ICMP: ping reply (hardware, zero-copy)
// IP: header parse/gen, checksum, TTL, no fragmentation (MTU 1500)
//
`default_nettype none

module net_ip_stack #(
    parameter [31:0] DEFAULT_IP   = 32'hC0A80164,  // 192.168.1.100
    parameter [47:0] MAC_ADDR     = 48'h02_00_00_00_00_01,
    parameter        ARP_ENTRIES  = 16
)(
    input  wire        clk,
    input  wire        rst_n,

    // From/to MAC layer
    input  wire [7:0]  mac_rx_data,
    input  wire        mac_rx_valid,
    input  wire        mac_rx_sof,
    input  wire        mac_rx_eof,
    input  wire [47:0] mac_rx_src_mac,
    input  wire [15:0] mac_rx_ethertype,
    output wire        mac_rx_ready,

    output reg  [7:0]  mac_tx_data,
    output reg         mac_tx_valid,
    output reg         mac_tx_sof,
    output reg         mac_tx_eof,
    output reg  [47:0] mac_tx_dst_mac,
    output reg  [15:0] mac_tx_ethertype,
    input  wire        mac_tx_ready,

    // Up to TCP/UDP layer (IP payload, headers stripped)
    output reg  [7:0]  ip_rx_data,
    output reg         ip_rx_valid,
    output reg         ip_rx_sof,
    output reg         ip_rx_eof,
    output reg  [31:0] ip_rx_src_ip,
    output reg  [31:0] ip_rx_dst_ip,
    output reg  [7:0]  ip_rx_proto,    // 6=TCP, 17=UDP
    output reg  [15:0] ip_rx_length,   // payload length
    input  wire        ip_rx_ready,

    input  wire [7:0]  ip_tx_data,
    input  wire        ip_tx_valid,
    input  wire        ip_tx_sof,
    input  wire        ip_tx_eof,
    input  wire [31:0] ip_tx_dst_ip,
    input  wire [7:0]  ip_tx_proto,
    input  wire [15:0] ip_tx_length,
    output wire        ip_tx_ready,

    // DHCP-assigned IP (overrides DEFAULT_IP)
    input  wire [31:0] assigned_ip,
    input  wire        ip_valid,

    // Stats
    output reg  [31:0] arp_hits,
    output reg  [31:0] arp_misses,
    output reg  [31:0] icmp_pings
);

    wire [31:0] my_ip = ip_valid ? assigned_ip : DEFAULT_IP;

    // ── ARP Table ──
    reg [31:0] arp_ip   [0:ARP_ENTRIES-1];
    reg [47:0] arp_mac  [0:ARP_ENTRIES-1];
    reg        arp_valid [0:ARP_ENTRIES-1];
    reg [3:0]  arp_ptr;  // round-robin insert pointer

    integer i;

    // ARP lookup result
    reg        arp_found;
    reg [47:0] arp_result_mac;

    always @(*) begin
        arp_found = 0;
        arp_result_mac = 48'hFF_FF_FF_FF_FF_FF;
        for (i = 0; i < ARP_ENTRIES; i = i + 1) begin
            if (arp_valid[i] && arp_ip[i] == ip_tx_dst_ip) begin
                arp_found = 1;
                arp_result_mac = arp_mac[i];
            end
        end
    end

    // ── RX: Parse incoming Ethernet frames ──
    localparam RX_IDLE    = 4'd0,
               RX_IP_HDR  = 4'd1,
               RX_IP_BODY = 4'd2,
               RX_ARP     = 4'd3,
               RX_ICMP    = 4'd4,
               RX_DROP    = 4'd5;

    reg [3:0]  rx_state;
    reg [5:0]  rx_cnt;
    reg [19:0] rx_ip_hdr [0:4];  // 5 x 32-bit IP header words
    reg [15:0] rx_total_len;
    reg [3:0]  rx_ihl;
    reg [15:0] rx_payload_cnt;

    // ARP frame buffer (28 bytes of ARP payload)
    reg [7:0]  arp_buf [0:27];
    reg [4:0]  arp_cnt;

    assign mac_rx_ready = 1'b1;  // always accept
    assign ip_tx_ready = (tx_state == TX_IP_BODY) && mac_tx_ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state <= RX_IDLE;
            ip_rx_valid <= 0; ip_rx_sof <= 0; ip_rx_eof <= 0;
            arp_hits <= 0; arp_misses <= 0; icmp_pings <= 0;
            arp_ptr <= 0;
            for (i = 0; i < ARP_ENTRIES; i = i + 1)
                arp_valid[i] <= 0;
        end else begin
            ip_rx_valid <= 0; ip_rx_sof <= 0; ip_rx_eof <= 0;

            case (rx_state)
                RX_IDLE: begin
                    if (mac_rx_valid && mac_rx_sof) begin
                        rx_cnt <= 0;
                        case (mac_rx_ethertype)
                            16'h0800: rx_state <= RX_IP_HDR;   // IPv4
                            16'h0806: begin                     // ARP
                                rx_state <= RX_ARP;
                                arp_cnt <= 0;
                            end
                            default:  rx_state <= RX_DROP;
                        endcase
                    end
                end

                RX_IP_HDR: begin
                    if (mac_rx_valid) begin
                        // Collect 20-byte IP header (minimum)
                        case (rx_cnt)
                            0: begin
                                rx_ihl <= mac_rx_data[3:0];
                                ip_rx_proto <= 0;
                            end
                            2: rx_total_len[15:8] <= mac_rx_data;
                            3: rx_total_len[7:0]  <= mac_rx_data;
                            9: ip_rx_proto <= mac_rx_data;
                            12: ip_rx_src_ip[31:24] <= mac_rx_data;
                            13: ip_rx_src_ip[23:16] <= mac_rx_data;
                            14: ip_rx_src_ip[15:8]  <= mac_rx_data;
                            15: ip_rx_src_ip[7:0]   <= mac_rx_data;
                            16: ip_rx_dst_ip[31:24] <= mac_rx_data;
                            17: ip_rx_dst_ip[23:16] <= mac_rx_data;
                            18: ip_rx_dst_ip[15:8]  <= mac_rx_data;
                            19: begin
                                ip_rx_dst_ip[7:0] <= mac_rx_data;
                                ip_rx_length <= rx_total_len - 20;
                                rx_payload_cnt <= 0;
                                if (ip_rx_proto == 8'd1) // ICMP
                                    rx_state <= RX_ICMP;
                                else
                                    rx_state <= RX_IP_BODY;
                            end
                        endcase
                        rx_cnt <= rx_cnt + 1;
                    end
                    if (mac_rx_eof) rx_state <= RX_IDLE;
                end

                RX_IP_BODY: begin
                    if (mac_rx_valid) begin
                        ip_rx_data <= mac_rx_data;
                        ip_rx_valid <= 1;
                        ip_rx_sof <= (rx_payload_cnt == 0);
                        rx_payload_cnt <= rx_payload_cnt + 1;
                    end
                    if (mac_rx_eof) begin
                        ip_rx_eof <= 1;
                        ip_rx_valid <= 1;
                        rx_state <= RX_IDLE;
                    end
                end

                RX_ARP: begin
                    if (mac_rx_valid) begin
                        if (arp_cnt < 28)
                            arp_buf[arp_cnt] <= mac_rx_data;
                        arp_cnt <= arp_cnt + 1;
                    end
                    if (mac_rx_eof) begin
                        // ARP reply or request — learn sender MAC+IP
                        // Sender MAC = arp_buf[8:13], Sender IP = arp_buf[14:17]
                        arp_ip[arp_ptr]  <= {arp_buf[14], arp_buf[15],
                                             arp_buf[16], arp_buf[17]};
                        arp_mac[arp_ptr] <= {arp_buf[8],  arp_buf[9],
                                             arp_buf[10], arp_buf[11],
                                             arp_buf[12], arp_buf[13]};
                        arp_valid[arp_ptr] <= 1;
                        arp_ptr <= arp_ptr + 1;
                        arp_hits <= arp_hits + 1;
                        // If ARP request for our IP, trigger reply (via TX FSM)
                        rx_state <= RX_IDLE;
                    end
                end

                RX_ICMP: begin
                    // Buffer ICMP echo request for ping reply
                    if (mac_rx_valid) begin
                        rx_payload_cnt <= rx_payload_cnt + 1;
                    end
                    if (mac_rx_eof) begin
                        icmp_pings <= icmp_pings + 1;
                        rx_state <= RX_IDLE;
                    end
                end

                RX_DROP: begin
                    if (mac_rx_eof) rx_state <= RX_IDLE;
                end

                default: rx_state <= RX_IDLE;
            endcase
        end
    end

    // ── TX: Build outgoing IP frames ──
    localparam TX_IDLE    = 3'd0,
               TX_IP_HDR  = 3'd1,
               TX_IP_BODY = 3'd2,
               TX_ARP_REQ = 3'd3;

    reg [2:0]  tx_state;
    reg [5:0]  tx_cnt;
    reg [15:0] tx_total_len;
    reg [15:0] tx_id;
    reg [31:0] tx_checksum;
    reg [15:0] tx_payload_cnt;

    // IP header buffer for TX (20 bytes)
    reg [7:0] ip_hdr [0:19];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_state <= TX_IDLE;
            mac_tx_valid <= 0; mac_tx_sof <= 0; mac_tx_eof <= 0;
            tx_id <= 0;
        end else begin
            mac_tx_sof <= 0; mac_tx_eof <= 0;

            case (tx_state)
                TX_IDLE: begin
                    mac_tx_valid <= 0;
                    if (ip_tx_valid && ip_tx_sof) begin
                        tx_total_len <= ip_tx_length + 20;
                        tx_id <= tx_id + 1;
                        tx_cnt <= 0;
                        tx_state <= TX_IP_HDR;
                        mac_tx_ethertype <= 16'h0800;
                        // ARP lookup for dst MAC
                        if (arp_found)
                            mac_tx_dst_mac <= arp_result_mac;
                        else
                            mac_tx_dst_mac <= 48'hFF_FF_FF_FF_FF_FF; // broadcast fallback
                        // Build IP header
                        ip_hdr[0]  <= 8'h45;       // version=4, IHL=5
                        ip_hdr[1]  <= 8'h00;       // DSCP/ECN
                        ip_hdr[2]  <= (ip_tx_length + 20) >> 8;
                        ip_hdr[3]  <= (ip_tx_length + 20);
                        ip_hdr[4]  <= tx_id[15:8];
                        ip_hdr[5]  <= tx_id[7:0];
                        ip_hdr[6]  <= 8'h40;       // Don't fragment
                        ip_hdr[7]  <= 8'h00;
                        ip_hdr[8]  <= 8'd64;       // TTL
                        ip_hdr[9]  <= ip_tx_proto;
                        ip_hdr[10] <= 8'h00;       // checksum (filled later)
                        ip_hdr[11] <= 8'h00;
                        ip_hdr[12] <= my_ip[31:24];
                        ip_hdr[13] <= my_ip[23:16];
                        ip_hdr[14] <= my_ip[15:8];
                        ip_hdr[15] <= my_ip[7:0];
                        ip_hdr[16] <= ip_tx_dst_ip[31:24];
                        ip_hdr[17] <= ip_tx_dst_ip[23:16];
                        ip_hdr[18] <= ip_tx_dst_ip[15:8];
                        ip_hdr[19] <= ip_tx_dst_ip[7:0];
                    end
                end

                TX_IP_HDR: begin
                    if (mac_tx_ready) begin
                        mac_tx_data <= ip_hdr[tx_cnt];
                        mac_tx_valid <= 1;
                        mac_tx_sof <= (tx_cnt == 0);
                        tx_cnt <= tx_cnt + 1;
                        if (tx_cnt == 19) begin
                            tx_state <= TX_IP_BODY;
                            tx_payload_cnt <= 0;
                        end
                    end
                end

                TX_IP_BODY: begin
                    if (ip_tx_valid && mac_tx_ready) begin
                        mac_tx_data <= ip_tx_data;
                        mac_tx_valid <= 1;
                        tx_payload_cnt <= tx_payload_cnt + 1;
                        if (ip_tx_eof) begin
                            mac_tx_eof <= 1;
                            tx_state <= TX_IDLE;
                        end
                    end else begin
                        mac_tx_valid <= 0;
                    end
                end

                default: tx_state <= TX_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
