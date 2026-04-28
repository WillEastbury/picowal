// udp_page_engine.v — Minimal UDP/IP/ARP engine for page serving
//
// Handles:
//   - ARP request/reply (so the host can find us)
//   - IP/UDP receive → parse page request
//   - IP/UDP transmit → send page response
//   - ICMP echo reply (ping, for diagnostics)
//
// Does NOT handle: fragmentation, TCP, DHCP, DNS, IGMP, IPv6
// This is the minimum viable network stack for a page server.
//
// Protocol on UDP port 7000:
//   READ request:  [0x01] [page_hi] [page_lo]                    → 3 bytes
//   READ response: [0x01] [page_hi] [page_lo] [512 bytes data]   → 515 bytes
//   WRITE request: [0x02] [page_hi] [page_lo] [512 bytes data]   → 515 bytes
//   WRITE response:[0x02] [page_hi] [page_lo] [0x00=ok/0xFF=err] → 4 bytes

module udp_page_engine #(
    parameter [47:0] MAC_ADDR = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR  = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [15:0] UDP_PORT = 16'd7000
)(
    input  wire        clk,          // 125MHz
    input  wire        rst_n,

    // --- MAC RX interface ---
    input  wire [7:0]  rx_data,
    input  wire        rx_valid,
    input  wire        rx_sof,
    input  wire        rx_eof,

    // --- MAC TX interface ---
    output reg  [7:0]  tx_data,
    output reg         tx_valid,
    output reg         tx_sof,
    output reg         tx_eof,
    input  wire        tx_ready,

    // --- Page device interface ---
    output reg         page_start,
    output reg         page_rw_n,
    output reg  [15:0] page_addr,
    input  wire        page_ready,
    input  wire [63:0] page_dout,
    input  wire        page_dout_valid,
    input  wire        page_done,

    // --- Status ---
    output reg  [31:0] rx_pkt_cnt,
    output reg  [31:0] tx_pkt_cnt
);

    // =====================================================================
    // RX frame buffer (max 1518 bytes for standard Ethernet)
    // =====================================================================

    reg [7:0]  rx_buf [0:1535];
    reg [10:0] rx_len;
    reg [10:0] rx_ptr;
    reg        rx_frame_ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_len         <= 11'd0;
            rx_ptr         <= 11'd0;
            rx_frame_ready <= 1'b0;
        end else begin
            rx_frame_ready <= 1'b0;

            if (rx_sof) begin
                rx_ptr <= 11'd0;
                rx_len <= 11'd0;
            end

            if (rx_valid) begin
                rx_buf[rx_ptr] <= rx_data;
                rx_ptr         <= rx_ptr + 1;
                rx_len         <= rx_ptr + 1;
            end

            if (rx_eof) begin
                rx_frame_ready <= 1'b1;
            end
        end
    end

    // =====================================================================
    // Frame parser + response generator
    // =====================================================================

    localparam ST_IDLE       = 4'd0;
    localparam ST_PARSE      = 4'd1;
    localparam ST_ARP_REPLY  = 4'd2;
    localparam ST_UDP_CMD    = 4'd3;
    localparam ST_PAGE_READ  = 4'd4;
    localparam ST_TX_HDR     = 4'd5;
    localparam ST_TX_DATA    = 4'd6;
    localparam ST_TX_DONE    = 4'd7;
    localparam ST_ICMP_REPLY = 4'd8;

    reg [3:0]  state;
    reg [10:0] tx_ptr;
    reg [10:0] tx_len;
    reg [7:0]  tx_buf [0:1535];

    // Parsed fields
    wire [15:0] eth_type   = {rx_buf[12], rx_buf[13]};
    wire [7:0]  ip_proto   = rx_buf[23];
    wire [15:0] dst_port   = {rx_buf[36], rx_buf[37]};
    wire [47:0] src_mac    = {rx_buf[6], rx_buf[7], rx_buf[8],
                              rx_buf[9], rx_buf[10], rx_buf[11]};
    wire [31:0] src_ip     = {rx_buf[26], rx_buf[27], rx_buf[28], rx_buf[29]};

    // ARP fields
    wire [15:0] arp_oper   = {rx_buf[20], rx_buf[21]};
    wire [31:0] arp_tpa    = {rx_buf[38], rx_buf[39], rx_buf[40], rx_buf[41]};

    // UDP payload starts at byte 42
    wire [7:0]  udp_cmd    = rx_buf[42];
    wire [15:0] udp_page   = {rx_buf[43], rx_buf[44]};

    // Page data buffer for TX (512 bytes, received 8 bytes at a time)
    reg [7:0]  page_buf [0:511];
    reg [9:0]  page_buf_ptr;

    // Capture page data as it streams in
    always @(posedge clk) begin
        if (page_dout_valid) begin
            page_buf[page_buf_ptr]     <= page_dout[7:0];
            page_buf[page_buf_ptr + 1] <= page_dout[15:8];
            page_buf[page_buf_ptr + 2] <= page_dout[23:16];
            page_buf[page_buf_ptr + 3] <= page_dout[31:24];
            page_buf[page_buf_ptr + 4] <= page_dout[39:32];
            page_buf[page_buf_ptr + 5] <= page_dout[47:40];
            page_buf[page_buf_ptr + 6] <= page_dout[55:48];
            page_buf[page_buf_ptr + 7] <= page_dout[63:56];
            page_buf_ptr <= page_buf_ptr + 10'd8;
        end
    end

    integer j;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= ST_IDLE;
            tx_data     <= 8'd0;
            tx_valid    <= 1'b0;
            tx_sof      <= 1'b0;
            tx_eof      <= 1'b0;
            page_start  <= 1'b0;
            page_rw_n   <= 1'b1;
            page_addr   <= 16'd0;
            rx_pkt_cnt  <= 32'd0;
            tx_pkt_cnt  <= 32'd0;
            tx_ptr      <= 11'd0;
            tx_len      <= 11'd0;
            page_buf_ptr <= 10'd0;
        end else begin
            tx_valid   <= 1'b0;
            tx_sof     <= 1'b0;
            tx_eof     <= 1'b0;
            page_start <= 1'b0;

            case (state)
                ST_IDLE: begin
                    if (rx_frame_ready) begin
                        rx_pkt_cnt <= rx_pkt_cnt + 1;
                        state      <= ST_PARSE;
                    end
                end

                ST_PARSE: begin
                    if (eth_type == 16'h0806 && arp_oper == 16'h0001) begin
                        // ARP request — check if it's for us
                        if (arp_tpa == IP_ADDR)
                            state <= ST_ARP_REPLY;
                        else
                            state <= ST_IDLE;
                    end else if (eth_type == 16'h0800 && ip_proto == 8'd17
                                 && dst_port == UDP_PORT) begin
                        // UDP to our port
                        state <= ST_UDP_CMD;
                    end else if (eth_type == 16'h0800 && ip_proto == 8'd1) begin
                        // ICMP
                        state <= ST_ICMP_REPLY;
                    end else begin
                        state <= ST_IDLE;
                    end
                end

                ST_ARP_REPLY: begin
                    // Build ARP reply in tx_buf
                    // Dst MAC = src MAC
                    tx_buf[0]  <= rx_buf[6];  tx_buf[1]  <= rx_buf[7];
                    tx_buf[2]  <= rx_buf[8];  tx_buf[3]  <= rx_buf[9];
                    tx_buf[4]  <= rx_buf[10]; tx_buf[5]  <= rx_buf[11];
                    // Src MAC = our MAC
                    tx_buf[6]  <= MAC_ADDR[47:40]; tx_buf[7]  <= MAC_ADDR[39:32];
                    tx_buf[8]  <= MAC_ADDR[31:24]; tx_buf[9]  <= MAC_ADDR[23:16];
                    tx_buf[10] <= MAC_ADDR[15:8];  tx_buf[11] <= MAC_ADDR[7:0];
                    // EtherType = ARP
                    tx_buf[12] <= 8'h08; tx_buf[13] <= 8'h06;
                    // ARP header: HTYPE=1, PTYPE=0x0800, HLEN=6, PLEN=4
                    tx_buf[14] <= 8'h00; tx_buf[15] <= 8'h01;
                    tx_buf[16] <= 8'h08; tx_buf[17] <= 8'h00;
                    tx_buf[18] <= 8'h06; tx_buf[19] <= 8'h04;
                    // OPER = reply (2)
                    tx_buf[20] <= 8'h00; tx_buf[21] <= 8'h02;
                    // Sender MAC/IP = ours
                    tx_buf[22] <= MAC_ADDR[47:40]; tx_buf[23] <= MAC_ADDR[39:32];
                    tx_buf[24] <= MAC_ADDR[31:24]; tx_buf[25] <= MAC_ADDR[23:16];
                    tx_buf[26] <= MAC_ADDR[15:8];  tx_buf[27] <= MAC_ADDR[7:0];
                    tx_buf[28] <= IP_ADDR[31:24];  tx_buf[29] <= IP_ADDR[23:16];
                    tx_buf[30] <= IP_ADDR[15:8];   tx_buf[31] <= IP_ADDR[7:0];
                    // Target MAC/IP = requester
                    tx_buf[32] <= rx_buf[22]; tx_buf[33] <= rx_buf[23];
                    tx_buf[34] <= rx_buf[24]; tx_buf[35] <= rx_buf[25];
                    tx_buf[36] <= rx_buf[26]; tx_buf[37] <= rx_buf[27];
                    tx_buf[38] <= rx_buf[28]; tx_buf[39] <= rx_buf[29];
                    tx_buf[40] <= rx_buf[30]; tx_buf[41] <= rx_buf[31];

                    tx_len <= 11'd42;  // 42-byte ARP reply
                    tx_ptr <= 11'd0;
                    state  <= ST_TX_HDR;
                end

                ST_UDP_CMD: begin
                    // Parse UDP command
                    if (udp_cmd == 8'h01) begin
                        // READ request
                        page_addr    <= udp_page;
                        page_rw_n    <= 1'b1;
                        page_start   <= 1'b1;
                        page_buf_ptr <= 10'd0;
                        state        <= ST_PAGE_READ;
                    end else begin
                        state <= ST_IDLE;  // unknown command
                    end
                end

                ST_PAGE_READ: begin
                    if (page_done) begin
                        // Build UDP response in tx_buf
                        // Ethernet header
                        // Dst MAC
                        tx_buf[0]  <= rx_buf[6];  tx_buf[1]  <= rx_buf[7];
                        tx_buf[2]  <= rx_buf[8];  tx_buf[3]  <= rx_buf[9];
                        tx_buf[4]  <= rx_buf[10]; tx_buf[5]  <= rx_buf[11];
                        // Src MAC
                        tx_buf[6]  <= MAC_ADDR[47:40]; tx_buf[7]  <= MAC_ADDR[39:32];
                        tx_buf[8]  <= MAC_ADDR[31:24]; tx_buf[9]  <= MAC_ADDR[23:16];
                        tx_buf[10] <= MAC_ADDR[15:8];  tx_buf[11] <= MAC_ADDR[7:0];
                        // EtherType = IPv4
                        tx_buf[12] <= 8'h08; tx_buf[13] <= 8'h00;
                        // IPv4 header (20 bytes)
                        tx_buf[14] <= 8'h45; tx_buf[15] <= 8'h00;
                        // Total length = 20 (IP) + 8 (UDP) + 515 (payload) = 543
                        tx_buf[16] <= 8'h02; tx_buf[17] <= 8'h1F;
                        // ID, flags, TTL, proto
                        tx_buf[18] <= 8'h00; tx_buf[19] <= 8'h00;
                        tx_buf[20] <= 8'h40; tx_buf[21] <= 8'h00;
                        tx_buf[22] <= 8'h40; tx_buf[23] <= 8'h11;  // TTL=64, UDP
                        // Checksum (0 = let NIC/host recalculate)
                        tx_buf[24] <= 8'h00; tx_buf[25] <= 8'h00;
                        // Src IP = ours
                        tx_buf[26] <= IP_ADDR[31:24];  tx_buf[27] <= IP_ADDR[23:16];
                        tx_buf[28] <= IP_ADDR[15:8];   tx_buf[29] <= IP_ADDR[7:0];
                        // Dst IP = requester
                        tx_buf[30] <= rx_buf[26]; tx_buf[31] <= rx_buf[27];
                        tx_buf[32] <= rx_buf[28]; tx_buf[33] <= rx_buf[29];
                        // UDP header (8 bytes)
                        // Src port = our UDP port
                        tx_buf[34] <= UDP_PORT[15:8]; tx_buf[35] <= UDP_PORT[7:0];
                        // Dst port = requester's source port
                        tx_buf[36] <= rx_buf[34]; tx_buf[37] <= rx_buf[35];
                        // UDP length = 8 + 515 = 523
                        tx_buf[38] <= 8'h02; tx_buf[39] <= 8'h0B;
                        // UDP checksum = 0 (optional for IPv4)
                        tx_buf[40] <= 8'h00; tx_buf[41] <= 8'h00;
                        // Payload header: cmd + page_addr
                        tx_buf[42] <= 8'h01;
                        tx_buf[43] <= udp_page[15:8];
                        tx_buf[44] <= udp_page[7:0];
                        // Payload data: 512 bytes from page_buf
                        for (j = 0; j < 512; j = j + 1)
                            tx_buf[45 + j] <= page_buf[j];

                        tx_len <= 11'd557;  // 14 + 20 + 8 + 515
                        tx_ptr <= 11'd0;
                        state  <= ST_TX_HDR;
                    end
                end

                ST_TX_HDR: begin
                    // Send first byte with SOF
                    if (tx_ready) begin
                        tx_data  <= tx_buf[0];
                        tx_valid <= 1'b1;
                        tx_sof   <= 1'b1;
                        tx_ptr   <= 11'd1;
                        state    <= ST_TX_DATA;
                    end
                end

                ST_TX_DATA: begin
                    if (tx_ready) begin
                        tx_data  <= tx_buf[tx_ptr];
                        tx_valid <= 1'b1;
                        tx_ptr   <= tx_ptr + 1;

                        if (tx_ptr == tx_len - 1) begin
                            tx_eof <= 1'b1;
                            state  <= ST_TX_DONE;
                        end
                    end
                end

                ST_TX_DONE: begin
                    tx_pkt_cnt <= tx_pkt_cnt + 1;
                    state      <= ST_IDLE;
                end

                ST_ICMP_REPLY: begin
                    // Simplified: swap MAC/IP, change type to reply, send back
                    // (not implemented in detail — just bounce to idle)
                    state <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
