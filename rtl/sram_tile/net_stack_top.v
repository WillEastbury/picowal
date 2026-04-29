// net_stack_top.v -- Full network stack, wired in layers
//
// Layer 1:  ECP5 SerDes hard IP (PCS) --------+
// Layer 2:  net_eth_mac    (MAC + FCS)         |  ~1200 LUTs
// Layer 3:  net_ip_stack   (IP + ARP + ICMP)   |  ~1500 LUTs
// Layer 3.5: proto_dhcp_dns (DHCP + DNS, UDP)  |  ~800 LUTs
// Layer 4:  net_tcp_engine (TCP, 64 sessions)  |  ~12000 LUTs
// Layer 5:  net_socket_mux (port routing)      |  ~500 LUTs
// Layer 6a: proto_smb2     (CIFS, port 445)    |  ~3500 LUTs
// Layer 6b: proto_http     (HTTP, port 80)     |  ~2500 LUTs
// Layer 6c: pico_bus       (DB/app, port 5432) |  (existing fpga_pico_bus.v)
//                                              |
// Total: ~22,000 LUTs per port                 |
// 2 ports on FPGA A = ~44,000 LUTs            |  fits in 85K ECP5
// Remaining: ~41,000 LUTs for NVMe + DSP + DMA
//
// No filesystem. Namespace is numeric:
//   SMB:  \\picowal\{card}\{folder}\{file}
//   HTTP: GET /{card}/{folder}/{file}
//   DB:   SELECT * FROM card.folder WHERE id = file
//
// Index pico resolves (card, folder, file) -> block address.
// NVMe DMA engine streams blocks. That's it.
//
`default_nettype none

module net_stack_top #(
    parameter [47:0] MAC_ADDR    = 48'h02_00_00_00_00_01,
    parameter [31:0] DEFAULT_IP  = 32'hC0A80164,
    parameter        PORT_ID     = 0  // 0 or 1 (which 2.5GbE port)
)(
    input  wire        clk,       // 125 MHz
    input  wire        rst_n,

    // ── SerDes PCS interface (from ECP5 hard macro) ──
    input  wire [7:0]  pcs_rxd,
    input  wire        pcs_rx_dv,
    input  wire        pcs_rx_er,
    output wire [7:0]  pcs_txd,
    output wire        pcs_tx_en,

    // ── NVMe DMA interface (shared across protocols) ──
    output wire        nvme_read_req,
    output wire        nvme_write_req,
    output wire [41:0] nvme_addr,
    output wire [15:0] nvme_count,
    input  wire        nvme_ready,
    input  wire [7:0]  nvme_rdata,
    input  wire        nvme_rvalid,
    input  wire        nvme_rdone,
    output wire [7:0]  nvme_wdata,
    output wire        nvme_wvalid,
    input  wire        nvme_wready,

    // ── Index pico interface ──
    output wire        idx_lookup_req,
    output wire [9:0]  idx_card,
    output wire [15:0] idx_folder,
    output wire [15:0] idx_file,
    input  wire        idx_lookup_ack,
    input  wire [41:0] idx_block_addr,
    input  wire [31:0] idx_file_size,
    input  wire        idx_not_found,

    // ── Session pico buses (directly exposed) ──
    output wire [7:0]  pico_rx_data,
    output wire        pico_rx_valid,
    output wire        pico_rx_sof,
    output wire        pico_rx_eof,
    output wire [5:0]  pico_rx_conn,
    input  wire [7:0]  pico_tx_data,
    input  wire        pico_tx_valid,
    input  wire        pico_tx_sof,
    input  wire        pico_tx_eof,
    input  wire [5:0]  pico_tx_conn,
    output wire        pico_tx_ready,

    // ── Listen ports ──
    input  wire [15:0] listen_port_0,
    input  wire [15:0] listen_port_1,
    input  wire [15:0] listen_port_2,
    input  wire [15:0] listen_port_3,

    // ── Status ──
    output wire [31:0] active_connections,
    output wire [31:0] total_connections,
    output wire [31:0] rx_frames,
    output wire [31:0] tx_frames,
    output wire [1:0]  dhcp_state
);

    // ════════════════════════════════════════════
    // Layer 2: MAC
    // ════════════════════════════════════════════
    wire [7:0]  mac_rx_data;
    wire        mac_rx_valid, mac_rx_sof, mac_rx_eof;
    wire [47:0] mac_rx_src_mac;
    wire [15:0] mac_rx_ethertype;
    wire        mac_rx_ready, mac_rx_err;

    wire [7:0]  mac_tx_data;
    wire        mac_tx_valid, mac_tx_sof, mac_tx_eof;
    wire [47:0] mac_tx_dst_mac;
    wire [15:0] mac_tx_ethertype;
    wire        mac_tx_ready;

    net_eth_mac #(.MAC_ADDR(MAC_ADDR)) u_mac (
        .clk(clk), .rst_n(rst_n),
        .pcs_rxd(pcs_rxd), .pcs_rx_dv(pcs_rx_dv), .pcs_rx_er(pcs_rx_er),
        .pcs_txd(pcs_txd), .pcs_tx_en(pcs_tx_en),
        .rx_data(mac_rx_data), .rx_valid(mac_rx_valid),
        .rx_sof(mac_rx_sof), .rx_eof(mac_rx_eof),
        .rx_src_mac(mac_rx_src_mac), .rx_ethertype(mac_rx_ethertype),
        .rx_err(mac_rx_err), .rx_ready(mac_rx_ready),
        .tx_data(mac_tx_data), .tx_valid(mac_tx_valid),
        .tx_sof(mac_tx_sof), .tx_eof(mac_tx_eof),
        .tx_dst_mac(mac_tx_dst_mac), .tx_ethertype(mac_tx_ethertype),
        .tx_ready(mac_tx_ready),
        .rx_frames(rx_frames), .tx_frames(tx_frames),
        .rx_crc_errs()
    );

    // ════════════════════════════════════════════
    // Layer 3: IP + ARP
    // ════════════════════════════════════════════
    wire [7:0]  ip_rx_data;
    wire        ip_rx_valid, ip_rx_sof, ip_rx_eof;
    wire [31:0] ip_rx_src_ip, ip_rx_dst_ip;
    wire [7:0]  ip_rx_proto;
    wire [15:0] ip_rx_length;
    wire        ip_rx_ready;

    wire [7:0]  ip_tx_data;
    wire        ip_tx_valid, ip_tx_sof, ip_tx_eof;
    wire [31:0] ip_tx_dst_ip;
    wire [7:0]  ip_tx_proto;
    wire [15:0] ip_tx_length;
    wire        ip_tx_ready;

    wire [31:0] dhcp_assigned_ip;
    wire        dhcp_ip_valid;

    net_ip_stack #(.DEFAULT_IP(DEFAULT_IP), .MAC_ADDR(MAC_ADDR)) u_ip (
        .clk(clk), .rst_n(rst_n),
        .mac_rx_data(mac_rx_data), .mac_rx_valid(mac_rx_valid),
        .mac_rx_sof(mac_rx_sof), .mac_rx_eof(mac_rx_eof),
        .mac_rx_src_mac(mac_rx_src_mac), .mac_rx_ethertype(mac_rx_ethertype),
        .mac_rx_ready(mac_rx_ready),
        .mac_tx_data(mac_tx_data), .mac_tx_valid(mac_tx_valid),
        .mac_tx_sof(mac_tx_sof), .mac_tx_eof(mac_tx_eof),
        .mac_tx_dst_mac(mac_tx_dst_mac), .mac_tx_ethertype(mac_tx_ethertype),
        .mac_tx_ready(mac_tx_ready),
        .ip_rx_data(ip_rx_data), .ip_rx_valid(ip_rx_valid),
        .ip_rx_sof(ip_rx_sof), .ip_rx_eof(ip_rx_eof),
        .ip_rx_src_ip(ip_rx_src_ip), .ip_rx_dst_ip(ip_rx_dst_ip),
        .ip_rx_proto(ip_rx_proto), .ip_rx_length(ip_rx_length),
        .ip_rx_ready(ip_rx_ready),
        .ip_tx_data(ip_tx_data), .ip_tx_valid(ip_tx_valid),
        .ip_tx_sof(ip_tx_sof), .ip_tx_eof(ip_tx_eof),
        .ip_tx_dst_ip(ip_tx_dst_ip), .ip_tx_proto(ip_tx_proto),
        .ip_tx_length(ip_tx_length), .ip_tx_ready(ip_tx_ready),
        .assigned_ip(dhcp_assigned_ip), .ip_valid(dhcp_ip_valid),
        .arp_hits(), .arp_misses(), .icmp_pings()
    );

    // ════════════════════════════════════════════
    // Layer 3.5: DHCP + DNS (UDP tap)
    // ════════════════════════════════════════════
    proto_dhcp_dns #(.MAC_ADDR(MAC_ADDR), .FALLBACK_IP(DEFAULT_IP)) u_dhcp_dns (
        .clk(clk), .rst_n(rst_n),
        .ip_rx_data(ip_rx_data), .ip_rx_valid(ip_rx_valid),
        .ip_rx_sof(ip_rx_sof), .ip_rx_eof(ip_rx_eof),
        .ip_rx_src_ip(ip_rx_src_ip), .ip_rx_proto(ip_rx_proto),
        .ip_rx_length(ip_rx_length),
        .ip_tx_data(), .ip_tx_valid(), .ip_tx_sof(), .ip_tx_eof(),
        .ip_tx_dst_ip(), .ip_tx_proto(), .ip_tx_length(),
        .ip_tx_ready(1'b1),
        .assigned_ip(dhcp_assigned_ip), .ip_assigned(dhcp_ip_valid),
        .gateway_ip(), .subnet_mask(), .dns_server_ip(),
        .dhcp_enable(1'b1), .dhcp_state_out(dhcp_state)
    );

    // ════════════════════════════════════════════
    // Layer 4: TCP (64 connections)
    // ════════════════════════════════════════════
    wire [7:0]  sock_rx_data;
    wire        sock_rx_valid, sock_rx_sof, sock_rx_eof;
    wire [5:0]  sock_rx_id;
    wire [15:0] sock_rx_port;
    wire        sock_rx_ready;

    wire [7:0]  sock_tx_data;
    wire        sock_tx_valid, sock_tx_sof, sock_tx_eof;
    wire [5:0]  sock_tx_id;
    wire        sock_tx_ready;

    net_tcp_engine #(.MAX_CONNECTIONS(64)) u_tcp (
        .clk(clk), .rst_n(rst_n),
        .ip_rx_data(ip_rx_data), .ip_rx_valid(ip_rx_valid),
        .ip_rx_sof(ip_rx_sof), .ip_rx_eof(ip_rx_eof),
        .ip_rx_src_ip(ip_rx_src_ip), .ip_rx_proto(ip_rx_proto),
        .ip_rx_length(ip_rx_length), .ip_rx_ready(ip_rx_ready),
        .ip_tx_data(ip_tx_data), .ip_tx_valid(ip_tx_valid),
        .ip_tx_sof(ip_tx_sof), .ip_tx_eof(ip_tx_eof),
        .ip_tx_dst_ip(ip_tx_dst_ip), .ip_tx_proto(ip_tx_proto),
        .ip_tx_length(ip_tx_length), .ip_tx_ready(ip_tx_ready),
        .sock_rx_data(sock_rx_data), .sock_rx_valid(sock_rx_valid),
        .sock_rx_sof(sock_rx_sof), .sock_rx_eof(sock_rx_eof),
        .sock_rx_id(sock_rx_id), .sock_rx_port(sock_rx_port),
        .sock_rx_ready(sock_rx_ready),
        .sock_tx_data(sock_tx_data), .sock_tx_valid(sock_tx_valid),
        .sock_tx_sof(sock_tx_sof), .sock_tx_eof(sock_tx_eof),
        .sock_tx_id(sock_tx_id), .sock_tx_ready(sock_tx_ready),
        .listen_port_0(listen_port_0), .listen_port_1(listen_port_1),
        .listen_port_2(listen_port_2), .listen_port_3(listen_port_3),
        .listen_port_4(16'd445),   // SMB2
        .listen_port_5(16'd80),    // HTTP
        .listen_port_6(16'd8080),  // HTTP alt
        .listen_port_7(16'd53),    // DNS
        .evt_connect(), .evt_conn_id(),
        .evt_disconnect(), .evt_disc_id(),
        .active_conns(active_connections),
        .total_conns(total_connections),
        .rx_bytes(), .tx_bytes()
    );

    // ════════════════════════════════════════════
    // Layer 5: Socket MUX (port-based routing)
    // ════════════════════════════════════════════
    wire [7:0]  smb_rx_data, http_rx_data, db_rx_data;
    wire        smb_rx_valid, http_rx_valid, db_rx_valid;
    wire        smb_rx_sof, http_rx_sof, db_rx_sof;
    wire        smb_rx_eof, http_rx_eof, db_rx_eof;
    wire [5:0]  smb_rx_conn, http_rx_conn, db_rx_conn;

    wire [7:0]  smb_tx_data, http_tx_data, db_tx_data;
    wire        smb_tx_valid, http_tx_valid, db_tx_valid;
    wire        smb_tx_sof, http_tx_sof, db_tx_sof;
    wire        smb_tx_eof, http_tx_eof, db_tx_eof;
    wire [5:0]  smb_tx_conn, http_tx_conn, db_tx_conn;
    wire        smb_tx_ready, http_tx_ready, db_tx_ready;

    net_socket_mux u_mux (
        .clk(clk), .rst_n(rst_n),
        .sock_rx_data(sock_rx_data), .sock_rx_valid(sock_rx_valid),
        .sock_rx_sof(sock_rx_sof), .sock_rx_eof(sock_rx_eof),
        .sock_rx_id(sock_rx_id), .sock_rx_port(sock_rx_port),
        .sock_rx_ready(sock_rx_ready),
        .sock_tx_data(sock_tx_data), .sock_tx_valid(sock_tx_valid),
        .sock_tx_sof(sock_tx_sof), .sock_tx_eof(sock_tx_eof),
        .sock_tx_id(sock_tx_id), .sock_tx_ready(sock_tx_ready),
        // SMB2
        .smb_rx_data(smb_rx_data), .smb_rx_valid(smb_rx_valid),
        .smb_rx_sof(smb_rx_sof), .smb_rx_eof(smb_rx_eof),
        .smb_rx_conn(smb_rx_conn),
        .smb_tx_data(smb_tx_data), .smb_tx_valid(smb_tx_valid),
        .smb_tx_sof(smb_tx_sof), .smb_tx_eof(smb_tx_eof),
        .smb_tx_conn(smb_tx_conn), .smb_tx_ready(smb_tx_ready),
        // HTTP
        .http_rx_data(http_rx_data), .http_rx_valid(http_rx_valid),
        .http_rx_sof(http_rx_sof), .http_rx_eof(http_rx_eof),
        .http_rx_conn(http_rx_conn),
        .http_tx_data(http_tx_data), .http_tx_valid(http_tx_valid),
        .http_tx_sof(http_tx_sof), .http_tx_eof(http_tx_eof),
        .http_tx_conn(http_tx_conn), .http_tx_ready(http_tx_ready),
        // DB
        .db_rx_data(db_rx_data), .db_rx_valid(db_rx_valid),
        .db_rx_sof(db_rx_sof), .db_rx_eof(db_rx_eof),
        .db_rx_conn(db_rx_conn),
        .db_tx_data(db_tx_data), .db_tx_valid(db_tx_valid),
        .db_tx_sof(db_tx_sof), .db_tx_eof(db_tx_eof),
        .db_tx_conn(db_tx_conn), .db_tx_ready(db_tx_ready),
        // Pico passthrough
        .pico_rx_data(pico_rx_data), .pico_rx_valid(pico_rx_valid),
        .pico_rx_sof(pico_rx_sof), .pico_rx_eof(pico_rx_eof),
        .pico_rx_conn(pico_rx_conn),
        .pico_tx_data(pico_tx_data), .pico_tx_valid(pico_tx_valid),
        .pico_tx_sof(pico_tx_sof), .pico_tx_eof(pico_tx_eof),
        .pico_tx_conn(pico_tx_conn), .pico_tx_ready(pico_tx_ready)
    );

    // ════════════════════════════════════════════
    // Layer 6a: SMB2/CIFS (port 445)
    // ════════════════════════════════════════════
    // NVMe arbitration wires for SMB2
    wire        smb_blk_read_req, smb_blk_write_req;
    wire [41:0] smb_blk_addr;
    wire [15:0] smb_blk_count;
    wire        smb_idx_req;
    wire [9:0]  smb_idx_card;
    wire [15:0] smb_idx_folder, smb_idx_file;

    proto_smb2 u_smb2 (
        .clk(clk), .rst_n(rst_n),
        .rx_data(smb_rx_data), .rx_valid(smb_rx_valid),
        .rx_sof(smb_rx_sof), .rx_eof(smb_rx_eof),
        .rx_conn(smb_rx_conn), .rx_ready(),
        .tx_data(smb_tx_data), .tx_valid(smb_tx_valid),
        .tx_sof(smb_tx_sof), .tx_eof(smb_tx_eof),
        .tx_conn(smb_tx_conn), .tx_ready(smb_tx_ready),
        .blk_read_req(smb_blk_read_req), .blk_write_req(smb_blk_write_req),
        .blk_addr(smb_blk_addr), .blk_count(smb_blk_count),
        .blk_ready(nvme_ready),
        .blk_rdata(nvme_rdata), .blk_rvalid(nvme_rvalid), .blk_rdone(nvme_rdone),
        .blk_wdata(), .blk_wvalid(), .blk_wready(nvme_wready),
        .idx_lookup_req(smb_idx_req),
        .idx_card(smb_idx_card), .idx_folder(smb_idx_folder), .idx_file(smb_idx_file),
        .idx_lookup_ack(idx_lookup_ack), .idx_block_addr(idx_block_addr),
        .idx_file_size(idx_file_size), .idx_not_found(idx_not_found),
        .smb_reads(), .smb_writes(), .smb_opens()
    );

    // ════════════════════════════════════════════
    // Layer 6b: HTTP (port 80)
    // ════════════════════════════════════════════
    wire        http_blk_read_req;
    wire [41:0] http_blk_addr;
    wire [15:0] http_blk_count;
    wire        http_idx_req;
    wire [9:0]  http_idx_card;
    wire [15:0] http_idx_folder, http_idx_file;

    proto_http u_http (
        .clk(clk), .rst_n(rst_n),
        .rx_data(http_rx_data), .rx_valid(http_rx_valid),
        .rx_sof(http_rx_sof), .rx_eof(http_rx_eof),
        .rx_conn(http_rx_conn), .rx_ready(),
        .tx_data(http_tx_data), .tx_valid(http_tx_valid),
        .tx_sof(http_tx_sof), .tx_eof(http_tx_eof),
        .tx_conn(http_tx_conn), .tx_ready(http_tx_ready),
        .blk_read_req(http_blk_read_req), .blk_addr(http_blk_addr),
        .blk_count(http_blk_count), .blk_ready(nvme_ready),
        .blk_rdata(nvme_rdata), .blk_rvalid(nvme_rvalid), .blk_rdone(nvme_rdone),
        .idx_lookup_req(http_idx_req),
        .idx_card(http_idx_card), .idx_folder(http_idx_folder), .idx_file(http_idx_file),
        .idx_lookup_ack(idx_lookup_ack), .idx_block_addr(idx_block_addr),
        .idx_file_size(idx_file_size), .idx_not_found(idx_not_found),
        .pico_tx_data(), .pico_tx_valid(), .pico_tx_sof(), .pico_tx_eof(),
        .pico_rx_data(8'h0), .pico_rx_valid(1'b0), .pico_rx_eof(1'b0),
        .http_gets(), .http_puts(), .http_404s()
    );

    // ════════════════════════════════════════════
    // NVMe + Index arbitration (simple priority: SMB > HTTP)
    // ════════════════════════════════════════════
    assign nvme_read_req  = smb_blk_read_req  | http_blk_read_req;
    assign nvme_write_req = smb_blk_write_req;
    assign nvme_addr      = smb_blk_read_req ? smb_blk_addr  : http_blk_addr;
    assign nvme_count     = smb_blk_read_req ? smb_blk_count : http_blk_count;
    assign nvme_wdata     = 8'h0;
    assign nvme_wvalid    = 1'b0;

    assign idx_lookup_req = smb_idx_req | http_idx_req;
    assign idx_card       = smb_idx_req ? smb_idx_card   : http_idx_card;
    assign idx_folder     = smb_idx_req ? smb_idx_folder : http_idx_folder;
    assign idx_file       = smb_idx_req ? smb_idx_file   : http_idx_file;

endmodule
`default_nettype wire
