// net_eth_mac.v -- Layer 2: Ethernet MAC for 2500BASE-X SerDes
// Sits on top of ECP5 SerDes hard IP (PCS provides 8b/10b)
// Provides byte-stream interface up to IP layer
//
// Stack position: SerDes PCS -> [THIS] -> net_ip_stack
//
`default_nettype none

module net_eth_mac #(
    parameter MAC_ADDR = 48'h02_00_00_00_00_01
)(
    input  wire        clk,          // 125MHz (2.5GbE: 312.5MHz/2.5 = byte clock)
    input  wire        rst_n,

    // SerDes PCS interface (from ECP5 hard IP)
    input  wire [7:0]  pcs_rxd,
    input  wire        pcs_rx_dv,     // data valid
    input  wire        pcs_rx_er,     // error
    output reg  [7:0]  pcs_txd,
    output reg         pcs_tx_en,

    // Upstream: to IP layer (stripped of preamble/SFD/FCS)
    output reg  [7:0]  rx_data,
    output reg         rx_valid,
    output reg         rx_sof,       // first byte of Ethernet payload
    output reg         rx_eof,       // last byte
    output reg  [47:0] rx_src_mac,
    output reg  [15:0] rx_ethertype,
    output reg         rx_err,       // FCS mismatch
    input  wire        rx_ready,

    // Downstream: from IP layer
    input  wire [7:0]  tx_data,
    input  wire        tx_valid,
    input  wire        tx_sof,
    input  wire        tx_eof,
    input  wire [47:0] tx_dst_mac,
    input  wire [15:0] tx_ethertype,
    output wire        tx_ready,

    // Stats
    output reg  [31:0] rx_frames,
    output reg  [31:0] tx_frames,
    output reg  [31:0] rx_crc_errs
);

    // ── RX State Machine ──
    localparam RX_IDLE     = 3'd0,
               RX_PREAMBLE = 3'd1,
               RX_DST_MAC  = 3'd2,
               RX_SRC_MAC  = 3'd3,
               RX_ETYPE    = 3'd4,
               RX_PAYLOAD  = 3'd5,
               RX_CHECK    = 3'd6;

    reg [2:0]  rx_state;
    reg [3:0]  rx_cnt;
    reg [47:0] rx_dst_buf;
    reg [31:0] rx_crc;
    reg [31:0] rx_fcs_got;
    reg [15:0] rx_len;

    // CRC32 for FCS
    wire [31:0] crc_next;
    reg         crc_en, crc_init;
    reg  [7:0]  crc_din;

    eth_crc32 u_rx_crc (
        .clk(clk), .rst_n(rst_n),
        .init(crc_init), .valid(crc_en),
        .data_in(crc_din), .crc_out(rx_crc)
    );

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state <= RX_IDLE;
            rx_valid <= 0; rx_sof <= 0; rx_eof <= 0; rx_err <= 0;
            rx_frames <= 0; rx_crc_errs <= 0;
            crc_en <= 0; crc_init <= 1;
        end else begin
            rx_valid <= 0; rx_sof <= 0; rx_eof <= 0; rx_err <= 0;
            crc_en <= 0; crc_init <= 0;

            case (rx_state)
                RX_IDLE: begin
                    if (pcs_rx_dv && pcs_rxd == 8'h55) begin
                        rx_state <= RX_PREAMBLE;
                        rx_cnt <= 0;
                        crc_init <= 1;
                    end
                end

                RX_PREAMBLE: begin
                    if (pcs_rx_dv) begin
                        if (pcs_rxd == 8'hD5) begin // SFD
                            rx_state <= RX_DST_MAC;
                            rx_cnt <= 0;
                        end else if (pcs_rxd != 8'h55) begin
                            rx_state <= RX_IDLE;
                        end
                    end else rx_state <= RX_IDLE;
                end

                RX_DST_MAC: begin
                    if (pcs_rx_dv) begin
                        crc_en <= 1; crc_din <= pcs_rxd;
                        rx_dst_buf <= {rx_dst_buf[39:0], pcs_rxd};
                        rx_cnt <= rx_cnt + 1;
                        if (rx_cnt == 5) begin
                            rx_state <= RX_SRC_MAC;
                            rx_cnt <= 0;
                        end
                    end else rx_state <= RX_IDLE;
                end

                RX_SRC_MAC: begin
                    if (pcs_rx_dv) begin
                        crc_en <= 1; crc_din <= pcs_rxd;
                        rx_src_mac <= {rx_src_mac[39:0], pcs_rxd};
                        rx_cnt <= rx_cnt + 1;
                        if (rx_cnt == 5) begin
                            rx_state <= RX_ETYPE;
                            rx_cnt <= 0;
                        end
                    end else rx_state <= RX_IDLE;
                end

                RX_ETYPE: begin
                    if (pcs_rx_dv) begin
                        crc_en <= 1; crc_din <= pcs_rxd;
                        rx_ethertype <= {rx_ethertype[7:0], pcs_rxd};
                        rx_cnt <= rx_cnt + 1;
                        if (rx_cnt == 1) begin
                            rx_state <= RX_PAYLOAD;
                            rx_len <= 0;
                        end
                    end else rx_state <= RX_IDLE;
                end

                RX_PAYLOAD: begin
                    if (pcs_rx_dv) begin
                        crc_en <= 1; crc_din <= pcs_rxd;
                        rx_data <= pcs_rxd;
                        rx_valid <= 1;
                        rx_sof <= (rx_len == 0);
                        rx_len <= rx_len + 1;
                    end else begin
                        // End of frame — check FCS (last 4 bytes were FCS)
                        rx_eof <= 1;
                        rx_valid <= 1;
                        if (rx_crc != 32'hC704DD7B) begin
                            rx_err <= 1;
                            rx_crc_errs <= rx_crc_errs + 1;
                        end
                        rx_frames <= rx_frames + 1;
                        rx_state <= RX_IDLE;
                    end
                end

                default: rx_state <= RX_IDLE;
            endcase
        end
    end

    // ── TX State Machine ──
    localparam TX_IDLE     = 3'd0,
               TX_PREAMBLE = 3'd1,
               TX_DST      = 3'd2,
               TX_SRC      = 3'd3,
               TX_ETYPE    = 3'd4,
               TX_PAYLOAD  = 3'd5,
               TX_FCS      = 3'd6,
               TX_IFG      = 3'd7;

    reg [2:0]  tx_state;
    reg [3:0]  tx_cnt;
    reg [31:0] tx_crc;
    reg        tx_crc_en, tx_crc_init;
    reg [7:0]  tx_crc_din;
    reg [47:0] tx_dst_r;
    reg [15:0] tx_etype_r;

    eth_crc32 u_tx_crc (
        .clk(clk), .rst_n(rst_n),
        .init(tx_crc_init), .valid(tx_crc_en),
        .data_in(tx_crc_din), .crc_out(tx_crc)
    );

    assign tx_ready = (tx_state == TX_PAYLOAD);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_state <= TX_IDLE;
            pcs_tx_en <= 0; pcs_txd <= 0;
            tx_frames <= 0;
            tx_crc_en <= 0; tx_crc_init <= 1;
        end else begin
            tx_crc_en <= 0; tx_crc_init <= 0;

            case (tx_state)
                TX_IDLE: begin
                    pcs_tx_en <= 0;
                    if (tx_valid && tx_sof) begin
                        tx_state <= TX_PREAMBLE;
                        tx_cnt <= 0;
                        tx_dst_r <= tx_dst_mac;
                        tx_etype_r <= tx_ethertype;
                        tx_crc_init <= 1;
                    end
                end

                TX_PREAMBLE: begin
                    pcs_tx_en <= 1;
                    pcs_txd <= (tx_cnt == 7) ? 8'hD5 : 8'h55;
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 7) begin
                        tx_state <= TX_DST;
                        tx_cnt <= 0;
                    end
                end

                TX_DST: begin
                    pcs_txd <= tx_dst_r[47:40];
                    tx_dst_r <= {tx_dst_r[39:0], 8'h0};
                    tx_crc_en <= 1; tx_crc_din <= tx_dst_r[47:40];
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 5) begin
                        tx_state <= TX_SRC;
                        tx_cnt <= 0;
                    end
                end

                TX_SRC: begin
                    pcs_txd <= MAC_ADDR[47 - tx_cnt*8 -: 8];
                    tx_crc_en <= 1;
                    tx_crc_din <= MAC_ADDR[47 - tx_cnt*8 -: 8];
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 5) begin
                        tx_state <= TX_ETYPE;
                        tx_cnt <= 0;
                    end
                end

                TX_ETYPE: begin
                    pcs_txd <= (tx_cnt == 0) ? tx_etype_r[15:8] : tx_etype_r[7:0];
                    tx_crc_en <= 1;
                    tx_crc_din <= (tx_cnt == 0) ? tx_etype_r[15:8] : tx_etype_r[7:0];
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 1) tx_state <= TX_PAYLOAD;
                end

                TX_PAYLOAD: begin
                    if (tx_valid) begin
                        pcs_txd <= tx_data;
                        tx_crc_en <= 1; tx_crc_din <= tx_data;
                        if (tx_eof) begin
                            tx_state <= TX_FCS;
                            tx_cnt <= 0;
                        end
                    end
                end

                TX_FCS: begin
                    case (tx_cnt)
                        0: pcs_txd <= tx_crc[31:24];
                        1: pcs_txd <= tx_crc[23:16];
                        2: pcs_txd <= tx_crc[15:8];
                        3: pcs_txd <= tx_crc[7:0];
                    endcase
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 3) begin
                        tx_state <= TX_IFG;
                        tx_cnt <= 0;
                        tx_frames <= tx_frames + 1;
                    end
                end

                TX_IFG: begin
                    pcs_tx_en <= 0;
                    tx_cnt <= tx_cnt + 1;
                    if (tx_cnt == 11) tx_state <= TX_IDLE; // 12-byte IFG
                end

                default: tx_state <= TX_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
