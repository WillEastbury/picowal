// sata_kv_node.v — M.2 SATA Network KV Storage Node
//
// The simplest possible design: FPGA bridges GbE ↔ M.2 SATA SSD
// Plug in any M.2 SATA drive (128GB to 4TB). Done.
//
// Architecture:
//   GbE RX → INGRESS FIFO → SATA CMD → M.2 SSD → SATA RESP → GbE TX
//
// The SSD handles wear leveling, bad blocks, FTL — we don't care.
// We just translate network requests to SATA READ/WRITE DMA commands.
//
// Instruction set (9 bytes over TCP, port 7000):
//   [flags:8] [address:64]
//   flags bit 0: 0=READ, 1=WRITE
//   address decomposition:
//     [63:53] tenant_id  (11b, 2048 tenants)
//     [52:42] card_id    (11b, 2048 cards)
//     [41:0]  block      (42b, per-card LBA)
//
//   READ:  → 4096 bytes response
//   WRITE: + 4096 bytes payload → 1-byte ACK
//
// Address mapping:
//   LBA = hash(uint64_addr) → 48-bit SATA LBA
//   NOR flash index maps virtual uint64 → physical LBA
//   OR: direct LBA mode (addr[41:0] × 8 sectors = LBA)
//
// Performance:
//   Cheap SATA SSD:    50-90K random IOPS, 500 MB/s sequential
//   GbE bottleneck:    29K IOPS @ 4KB, 117 MB/s
//   Result:            GbE saturated on all workloads
//
// SATA-II over ECP5 SERDES:
//   ECP5 SERDES: 3.125 Gbps max → SATA-II 3.0 Gbps (300 MB/s)
//   300 MB/s > 117 MB/s GbE → SSD never the bottleneck
//   Uses 1× SERDES channel (TX+RX differential pair = 4 pins)
//
// Pin budget (ECP5 LFE5U-25F BGA-256):
//   SATA SERDES:       4 pins (TX+/-, RX+/-)
//   RGMII GbE:         12
//   PHY mgmt:          3
//   NOR flash (opt):   42 (only if tenant mapping needed)
//   M.2 connector:     SATA signals + power
//   LEDs:              4
//   Crystal:           2
//   Total:             25-67 pins — trivial
//
// Power: USB-C 5V (any phone charger works, ~4W total draw)
//   ECP5:      ~1W
//   GbE PHY:   ~0.5W
//   M.2 SSD:   ~2-3W
//   Total:     ~3.5-4.5W (USB-C 5V/1.5A = 7.5W headroom)
//
// BOM:
//   ECP5 LFE5U-25F          $10
//   RTL8211F GbE PHY         $2.50
//   M.2 SATA connector       $1
//   USB-C connector           $0.50
//   LDO regulators (3.3V+1.2V) $1.50
//   RJ45 + magnetics         $3
//   Crystal + passives       $5
//   PCB (4-layer, SERDES)    $8
//   ─────────────────────────────
//   Node (without drive):    $31.50
//
//   + 128GB M.2 SATA SSD:   ~$15  (dev/test)
//   ═════════════════════════════
//   TOTAL:                   ~$47 ≈ £37
//
//   + 1TB M.2 SATA SSD:     ~$45  (production)
//   TOTAL:                   ~$77 ≈ £61
//
//   + 2TB M.2 SATA SSD:     ~$80
//   TOTAL:                   ~$112 ≈ £89
//
//   + 4TB M.2 SATA SSD:     ~$150
//   TOTAL:                   ~$182 ≈ £145
//
// Dev kit: 128GB network KV store for £37. Swap drive for instant upgrade.

module sata_kv_node #(
    parameter [47:0] MAC_ADDR = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR  = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [15:0] KV_PORT  = 16'd7000,
    parameter PAGE_SIZE       = 4096,
    parameter SECTORS_PER_PAGE = 8      // 4096 / 512
)(
    input  wire        clk_12m,        // 12MHz crystal input

    // --- RGMII PHY (GbE) ---
    output wire        rgmii_txc,
    output wire [3:0]  rgmii_txd,
    output wire        rgmii_tx_ctl,
    input  wire        rgmii_rxc,
    input  wire [3:0]  rgmii_rxd,
    input  wire        rgmii_rx_ctl,
    output wire        phy_rst_n,
    output wire        mdc,
    inout  wire        mdio,

    // --- SATA (ECP5 SERDES channel 0) ---
    output wire        sata_txp,
    output wire        sata_txn,
    input  wire        sata_rxp,
    input  wire        sata_rxn,

    // --- Optional NOR flash index ---
    output wire [21:0] nor_addr,
    inout  wire [15:0] nor_dq,
    output wire        nor_ce_n,
    output wire        nor_oe_n,
    output wire        nor_we_n,

    // --- Status ---
    output wire        led_link,
    output wire        led_sata_rdy,
    output wire        led_activity,
    output wire        led_error
);

    // =====================================================================
    // PLL: 12MHz → 125MHz (GbE) + 150MHz (SATA PHY ref) + 50MHz (logic)
    // =====================================================================

    wire clk_125m, clk_150m, clk_50m;
    wire pll_lock;

    // In real design: ECP5 PLL primitive (EHXPLLL)
    // For simulation, just use input clock
    assign clk_50m  = clk_12m;
    assign clk_125m = clk_12m;
    assign clk_150m = clk_12m;
    assign pll_lock = 1'b1;

    // =====================================================================
    // Reset sequencing
    // =====================================================================

    wire rst_n = pll_lock;

    // =====================================================================
    // SERDES device-side OOB mock (simulation stub)
    // In production: ECP5 DCU primitive connects sata_txp/txn/rxp/rxn
    // =====================================================================

    wire [31:0] serdes_tx_data;
    wire [3:0]  serdes_tx_charisk;
    wire        serdes_tx_comreset;
    wire        serdes_tx_comwake_o;

    // Track end-of-burst for COMRESET and COMWAKE
    reg        saw_comreset, saw_comwake;
    reg  [7:0] oob_idle_cnt;
    reg  [2:0] mock_state;
    reg  [7:0] mock_timer;

    localparam MOCK_WAIT_COMRESET = 3'd0,
               MOCK_REPLY_COMINIT = 3'd1,
               MOCK_WAIT_COMWAKE  = 3'd2,
               MOCK_REPLY_COMWAKE = 3'd3,
               MOCK_ALIGNED       = 3'd4;
    localparam OOB_IDLE_THRESH    = 8;

    always @(posedge clk_150m or negedge rst_n) begin
        if (!rst_n) begin
            mock_state   <= MOCK_WAIT_COMRESET;
            oob_idle_cnt <= 8'd0;
            mock_timer   <= 8'd0;
            saw_comreset <= 1'b0;
            saw_comwake  <= 1'b0;
        end else begin
            case (mock_state)
                MOCK_WAIT_COMRESET: begin
                    if (serdes_tx_comreset) begin
                        oob_idle_cnt <= 8'd0;
                        saw_comreset <= 1'b1;
                    end else if (saw_comreset) begin
                        oob_idle_cnt <= oob_idle_cnt + 8'd1;
                        if (oob_idle_cnt == OOB_IDLE_THRESH[7:0]) begin
                            mock_state <= MOCK_REPLY_COMINIT;
                            mock_timer <= 8'd15;
                        end
                    end
                end
                MOCK_REPLY_COMINIT: begin
                    if (mock_timer > 0) mock_timer <= mock_timer - 8'd1;
                    else begin
                        mock_state   <= MOCK_WAIT_COMWAKE;
                        oob_idle_cnt <= 8'd0;
                        saw_comwake  <= 1'b0;
                    end
                end
                MOCK_WAIT_COMWAKE: begin
                    if (serdes_tx_comwake_o) begin
                        oob_idle_cnt <= 8'd0;
                        saw_comwake  <= 1'b1;
                    end else if (saw_comwake) begin
                        oob_idle_cnt <= oob_idle_cnt + 8'd1;
                        if (oob_idle_cnt == OOB_IDLE_THRESH[7:0]) begin
                            mock_state <= MOCK_REPLY_COMWAKE;
                            mock_timer <= 8'd15;
                        end
                    end
                end
                MOCK_REPLY_COMWAKE: begin
                    if (mock_timer > 0) mock_timer <= mock_timer - 8'd1;
                    else mock_state <= MOCK_ALIGNED;
                end
                MOCK_ALIGNED: begin
                    // stay here — device aligned
                end
                default: mock_state <= MOCK_WAIT_COMRESET;
            endcase
        end
    end

    wire mock_rx_cominit = (mock_state == MOCK_REPLY_COMINIT);
    wire mock_rx_comwake = (mock_state == MOCK_REPLY_COMWAKE);
    wire mock_rx_aligned = (mock_state == MOCK_ALIGNED);

    assign sata_txp = 1'b0;
    assign sata_txn = 1'b1;

    // =====================================================================
    // SATA PHY Layer
    // =====================================================================

    wire        w_phy_ready;
    wire [1:0]  w_phy_speed;
    wire [31:0] w_phy_link_rx_data;
    wire [3:0]  w_phy_link_rx_isk;
    wire        w_phy_link_rx_valid;
    wire [31:0] w_link_phy_tx_data;
    wire [3:0]  w_link_phy_tx_isk;

    sata_phy #(
        .OOB_SHORT (1)
    ) u_phy (
        .clk             (clk_150m),
        .rst_n           (rst_n),
        // SERDES side (loopback + OOB mock)
        .tx_data         (serdes_tx_data),
        .tx_charisk      (serdes_tx_charisk),
        .tx_comreset     (serdes_tx_comreset),
        .tx_comwake      (serdes_tx_comwake_o),
        .rx_data         (serdes_tx_data),       // loopback for ALIGN
        .rx_charisk      (serdes_tx_charisk),    // loopback for ALIGN
        .rx_cominit      (mock_rx_cominit),
        .rx_comwake      (mock_rx_comwake),
        .rx_byte_aligned (mock_rx_aligned),
        // Link side
        .phy_ready       (w_phy_ready),
        .phy_speed       (w_phy_speed),
        .link_tx_data    (w_link_phy_tx_data),
        .link_tx_isk     (w_link_phy_tx_isk),
        .link_tx_valid   (1'b1),
        .link_rx_data    (w_phy_link_rx_data),
        .link_rx_isk     (w_phy_link_rx_isk),
        .link_rx_valid   (w_phy_link_rx_valid)
    );

    // =====================================================================
    // SATA Link Layer
    // =====================================================================

    wire [31:0] w_tp_link_tx_data;
    wire        w_tp_link_tx_valid;
    wire        w_tp_link_tx_last;
    wire        w_tp_link_tx_start;
    wire        w_link_tx_ready;
    wire        w_link_tx_done;
    wire        w_link_tx_err;
    wire [31:0] w_link_rx_data;
    wire        w_link_rx_valid;
    wire        w_link_rx_last;
    wire        w_link_rx_sof;
    wire        w_link_rx_err;

    sata_link u_link (
        .clk         (clk_150m),
        .rst_n       (rst_n),
        .phy_tx_data (w_link_phy_tx_data),
        .phy_tx_isk  (w_link_phy_tx_isk),
        .phy_rx_data (w_phy_link_rx_data),
        .phy_rx_isk  (w_phy_link_rx_isk),
        .phy_ready   (w_phy_ready),
        .tx_data     (w_tp_link_tx_data),
        .tx_valid    (w_tp_link_tx_valid),
        .tx_last     (w_tp_link_tx_last),
        .tx_ready    (w_link_tx_ready),
        .tx_start    (w_tp_link_tx_start),
        .tx_done     (w_link_tx_done),
        .tx_err      (w_link_tx_err),
        .rx_data     (w_link_rx_data),
        .rx_valid    (w_link_rx_valid),
        .rx_last     (w_link_rx_last),
        .rx_sof      (w_link_rx_sof),
        .rx_err      (w_link_rx_err)
    );

    // =====================================================================
    // SATA Transport Layer
    // =====================================================================

    wire        w_cmd_tp_cmd_start;
    wire [7:0]  w_cmd_tp_cmd_command;
    wire [47:0] w_cmd_tp_cmd_lba;
    wire [15:0] w_cmd_tp_cmd_count;
    wire [7:0]  w_cmd_tp_cmd_features;
    wire [7:0]  w_cmd_tp_cmd_device;
    wire        w_tp_cmd_done;
    wire        w_tp_cmd_err;
    wire        w_cmd_tp_data_start;
    wire [31:0] w_cmd_tp_data_dword;
    wire        w_cmd_tp_data_valid;
    wire        w_cmd_tp_data_last;
    wire        w_tp_data_ready;
    wire        w_tp_data_done;
    wire        w_tp_rx_reg_fis_valid;
    wire [7:0]  w_tp_rx_status;
    wire [7:0]  w_tp_rx_error;
    wire        w_tp_rx_pio_setup_valid;
    wire [15:0] w_tp_rx_pio_xfer_count;
    wire [7:0]  w_tp_rx_pio_status;
    wire        w_tp_rx_dma_activate;
    wire [31:0] w_tp_rx_data_dword;
    wire        w_tp_rx_data_valid;
    wire        w_tp_rx_data_last;
    wire        w_tp_rx_data_err;

    sata_transport u_transport (
        .clk                (clk_150m),
        .rst_n              (rst_n),
        .link_tx_data       (w_tp_link_tx_data),
        .link_tx_valid      (w_tp_link_tx_valid),
        .link_tx_last       (w_tp_link_tx_last),
        .link_tx_ready      (w_link_tx_ready),
        .link_tx_start      (w_tp_link_tx_start),
        .link_tx_done       (w_link_tx_done),
        .link_tx_err        (w_link_tx_err),
        .link_rx_data       (w_link_rx_data),
        .link_rx_valid      (w_link_rx_valid),
        .link_rx_last       (w_link_rx_last),
        .link_rx_sof        (w_link_rx_sof),
        .link_rx_err        (w_link_rx_err),
        .cmd_tx_start       (w_cmd_tp_cmd_start),
        .cmd_tx_command     (w_cmd_tp_cmd_command),
        .cmd_tx_lba         (w_cmd_tp_cmd_lba),
        .cmd_tx_count       (w_cmd_tp_cmd_count),
        .cmd_tx_features    (w_cmd_tp_cmd_features),
        .cmd_tx_device      (w_cmd_tp_cmd_device),
        .cmd_tx_done        (w_tp_cmd_done),
        .cmd_tx_err         (w_tp_cmd_err),
        .data_tx_start      (w_cmd_tp_data_start),
        .data_tx_dword      (w_cmd_tp_data_dword),
        .data_tx_valid      (w_cmd_tp_data_valid),
        .data_tx_last       (w_cmd_tp_data_last),
        .data_tx_ready      (w_tp_data_ready),
        .data_tx_done       (w_tp_data_done),
        .rx_reg_fis_valid   (w_tp_rx_reg_fis_valid),
        .rx_status          (w_tp_rx_status),
        .rx_error           (w_tp_rx_error),
        .rx_pio_setup_valid (w_tp_rx_pio_setup_valid),
        .rx_pio_xfer_count  (w_tp_rx_pio_xfer_count),
        .rx_pio_status      (w_tp_rx_pio_status),
        .rx_dma_activate    (w_tp_rx_dma_activate),
        .rx_data_dword      (w_tp_rx_data_dword),
        .rx_data_valid      (w_tp_rx_data_valid),
        .rx_data_last       (w_tp_rx_data_last),
        .rx_data_err        (w_tp_rx_data_err)
    );

    // =====================================================================
    // SATA Command Layer
    // =====================================================================

    wire        w_kv_cmd_valid;
    wire        w_cmd_user_ready;
    wire        w_kv_cmd_is_write;
    wire [47:0] w_kv_cmd_lba;
    wire [15:0] w_kv_cmd_count;
    wire [31:0] w_kv_wr_data;
    wire        w_kv_wr_valid;
    wire        w_kv_wr_last;
    wire        w_cmd_wr_ready;
    wire [31:0] w_cmd_rd_data;
    wire        w_cmd_rd_valid;
    wire        w_cmd_rd_last;
    wire        w_cmd_complete;
    wire        w_cmd_error;
    wire [7:0]  w_cmd_status_out;
    wire        w_init_done;
    wire [127:0] w_identify_data;

    // Trigger init on rising edge of phy_ready
    reg phy_ready_d;
    always @(posedge clk_150m or negedge rst_n)
        if (!rst_n) phy_ready_d <= 1'b0;
        else        phy_ready_d <= w_phy_ready;
    wire w_do_init = w_phy_ready & ~phy_ready_d;

    sata_command u_command (
        .clk                   (clk_150m),
        .rst_n                 (rst_n),
        .user_cmd_valid        (w_kv_cmd_valid),
        .user_cmd_ready        (w_cmd_user_ready),
        .user_cmd_is_write     (w_kv_cmd_is_write),
        .user_cmd_lba          (w_kv_cmd_lba),
        .user_cmd_count        (w_kv_cmd_count),
        .user_wr_data          (w_kv_wr_data),
        .user_wr_valid         (w_kv_wr_valid),
        .user_wr_last          (w_kv_wr_last),
        .user_wr_ready         (w_cmd_wr_ready),
        .user_rd_data          (w_cmd_rd_data),
        .user_rd_valid         (w_cmd_rd_valid),
        .user_rd_last          (w_cmd_rd_last),
        .cmd_complete          (w_cmd_complete),
        .cmd_error             (w_cmd_error),
        .cmd_status            (w_cmd_status_out),
        .do_init               (w_do_init),
        .init_done             (w_init_done),
        .identify_data_flat    (w_identify_data),
        .tp_cmd_tx_start       (w_cmd_tp_cmd_start),
        .tp_cmd_tx_command     (w_cmd_tp_cmd_command),
        .tp_cmd_tx_lba         (w_cmd_tp_cmd_lba),
        .tp_cmd_tx_count       (w_cmd_tp_cmd_count),
        .tp_cmd_tx_features    (w_cmd_tp_cmd_features),
        .tp_cmd_tx_device      (w_cmd_tp_cmd_device),
        .tp_cmd_tx_done        (w_tp_cmd_done),
        .tp_cmd_tx_err         (w_tp_cmd_err),
        .tp_data_tx_start      (w_cmd_tp_data_start),
        .tp_data_tx_dword      (w_cmd_tp_data_dword),
        .tp_data_tx_valid      (w_cmd_tp_data_valid),
        .tp_data_tx_last       (w_cmd_tp_data_last),
        .tp_data_tx_ready      (w_tp_data_ready),
        .tp_data_tx_done       (w_tp_data_done),
        .tp_rx_reg_fis_valid   (w_tp_rx_reg_fis_valid),
        .tp_rx_status          (w_tp_rx_status),
        .tp_rx_error           (w_tp_rx_error),
        .tp_rx_pio_setup_valid (w_tp_rx_pio_setup_valid),
        .tp_rx_pio_xfer_count  (w_tp_rx_pio_xfer_count),
        .tp_rx_pio_status      (w_tp_rx_pio_status),
        .tp_rx_dma_activate    (w_tp_rx_dma_activate),
        .tp_rx_data_dword      (w_tp_rx_data_dword),
        .tp_rx_data_valid      (w_tp_rx_data_valid),
        .tp_rx_data_last       (w_tp_rx_data_last),
        .tp_rx_data_err        (w_tp_rx_data_err)
    );

    // =====================================================================
    // Ingress FIFO: 72-bit entries [71:64]=flags [63:0]=uint64 address
    // =====================================================================

    reg [71:0] ingress_fifo [0:255];
    reg [7:0]  ingress_wr, ingress_rd;
    wire       ingress_empty = (ingress_wr == ingress_rd);
    wire       ingress_full  = ((ingress_wr + 8'd1) == ingress_rd);

    // Network RX stub — no real MAC yet
    wire        ingress_push = 1'b0;
    wire [71:0] ingress_din  = 72'd0;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n)
            ingress_wr <= 8'd0;
        else if (ingress_push && !ingress_full) begin
            ingress_fifo[ingress_wr] <= ingress_din;
            ingress_wr <= ingress_wr + 8'd1;
        end
    end

    // =====================================================================
    // Ingress FIFO → KV Engine adapter
    // =====================================================================

    wire w_kv_cmd_ready;

    reg        fifo_cmd_valid;
    reg [7:0]  fifo_cmd_flags;
    reg [63:0] fifo_cmd_addr;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            fifo_cmd_valid <= 1'b0;
            ingress_rd     <= 8'd0;
        end else begin
            if (fifo_cmd_valid && w_kv_cmd_ready)
                fifo_cmd_valid <= 1'b0;
            if (!fifo_cmd_valid && !ingress_empty) begin
                fifo_cmd_flags <= ingress_fifo[ingress_rd][71:64];
                fifo_cmd_addr  <= ingress_fifo[ingress_rd][63:0];
                fifo_cmd_valid <= 1'b1;
                ingress_rd     <= ingress_rd + 8'd1;
            end
        end
    end

    // =====================================================================
    // KV Engine
    // =====================================================================

    wire        w_kv_net_wr_ready;
    wire [31:0] w_kv_resp_data;
    wire        w_kv_resp_valid;
    wire        w_kv_resp_last;
    wire        w_kv_resp_start;
    wire [31:0] w_kv_resp_tag;
    wire [7:0]  w_kv_resp_flags;
    wire        w_kv_ack_valid;
    wire [7:0]  w_kv_ack_byte;
    wire [31:0] w_kv_reads_completed;
    wire [31:0] w_kv_writes_completed;
    wire [31:0] w_kv_errors;

    // Network write data (for WRITE commands) — stubbed, no real MAC
    wire [31:0] net_wr_data_stub  = 32'd0;
    wire        net_wr_valid_stub = 1'b0;
    wire        net_wr_last_stub  = 1'b0;

    // Response backpressure — always accept
    wire w_kv_resp_ready = 1'b1;

    sata_kv_engine u_kv (
        .clk               (clk_50m),
        .rst_n             (rst_n),
        .cmd_valid         (fifo_cmd_valid),
        .cmd_flags         (fifo_cmd_flags),
        .cmd_addr          (fifo_cmd_addr),
        .cmd_ready         (w_kv_cmd_ready),
        .net_wr_data       (net_wr_data_stub),
        .net_wr_valid      (net_wr_valid_stub),
        .net_wr_last       (net_wr_last_stub),
        .net_wr_ready      (w_kv_net_wr_ready),
        .sata_cmd_valid    (w_kv_cmd_valid),
        .sata_cmd_ready    (w_cmd_user_ready),
        .sata_cmd_is_write (w_kv_cmd_is_write),
        .sata_cmd_lba      (w_kv_cmd_lba),
        .sata_cmd_count    (w_kv_cmd_count),
        .sata_wr_data      (w_kv_wr_data),
        .sata_wr_valid     (w_kv_wr_valid),
        .sata_wr_last      (w_kv_wr_last),
        .sata_wr_ready     (w_cmd_wr_ready),
        .sata_rd_data      (w_cmd_rd_data),
        .sata_rd_valid     (w_cmd_rd_valid),
        .sata_rd_last      (w_cmd_rd_last),
        .sata_cmd_complete (w_cmd_complete),
        .sata_cmd_error    (w_cmd_error),
        .sata_cmd_status   (w_cmd_status_out),
        .resp_data         (w_kv_resp_data),
        .resp_valid        (w_kv_resp_valid),
        .resp_last         (w_kv_resp_last),
        .resp_ready        (w_kv_resp_ready),
        .resp_start        (w_kv_resp_start),
        .resp_tag          (w_kv_resp_tag),
        .resp_flags        (w_kv_resp_flags),
        .ack_valid         (w_kv_ack_valid),
        .ack_byte          (w_kv_ack_byte),
        .reads_completed   (w_kv_reads_completed),
        .writes_completed  (w_kv_writes_completed),
        .errors            (w_kv_errors)
    );

    // =====================================================================
    // Response page buffer (KV engine writes, udp_page_tx reads)
    // =====================================================================

    reg [7:0]  page_buf [0:4095];
    reg [11:0] page_wr_addr;

    always @(posedge clk_50m) begin
        if (w_kv_resp_start)
            page_wr_addr <= 12'd0;
        else if (w_kv_resp_valid) begin
            page_buf[page_wr_addr]         <= w_kv_resp_data[7:0];
            page_buf[page_wr_addr + 12'd1] <= w_kv_resp_data[15:8];
            page_buf[page_wr_addr + 12'd2] <= w_kv_resp_data[23:16];
            page_buf[page_wr_addr + 12'd3] <= w_kv_resp_data[31:24];
            page_wr_addr <= page_wr_addr + 12'd4;
        end
    end

    wire [11:0] tx_buf_addr;
    wire [7:0]  tx_buf_rdata = page_buf[tx_buf_addr];

    // =====================================================================
    // UDP Page TX (network fragmenter)
    // =====================================================================

    // Pulse start when KV engine finishes a response page
    reg tx_start_pulse;
    always @(posedge clk_50m or negedge rst_n)
        if (!rst_n) tx_start_pulse <= 1'b0;
        else        tx_start_pulse <= w_kv_resp_valid & w_kv_resp_last;

    wire       tx_frag_done;
    wire       tx_frag_busy;
    wire [7:0] tx_byte_data;
    wire       tx_byte_valid, tx_byte_sof, tx_byte_eof;

    udp_page_tx u_page_tx (
        .clk        (clk_50m),
        .rst_n      (rst_n),
        .buf_addr   (tx_buf_addr),
        .buf_data   (tx_buf_rdata),
        .start      (tx_start_pulse),
        .req_tag    (w_kv_resp_tag),
        .resp_flags (w_kv_resp_flags),
        .done       (tx_frag_done),
        .busy       (tx_frag_busy),
        .tx_data    (tx_byte_data),
        .tx_valid   (tx_byte_valid),
        .tx_sof     (tx_byte_sof),
        .tx_eof     (tx_byte_eof),
        .tx_ready   (1'b1)          // stub: always accept
    );

    // =====================================================================
    // NOR flash (unused in direct LBA mode)
    // =====================================================================

    assign nor_addr = 22'd0;
    assign nor_dq   = 16'hzzzz;
    assign nor_ce_n = 1'b1;
    assign nor_oe_n = 1'b1;
    assign nor_we_n = 1'b1;

    // =====================================================================
    // LED status
    // =====================================================================

    assign led_link     = 1'b0;           // TODO: GbE link detect
    assign led_sata_rdy = w_phy_ready;
    assign led_activity = ~ingress_empty | tx_frag_busy;
    assign led_error    = w_cmd_error;

    // =====================================================================
    // RGMII / PHY management stubs
    // =====================================================================

    assign phy_rst_n    = rst_n;
    assign mdc          = 1'b0;
    assign mdio         = 1'bz;
    assign rgmii_txc    = clk_125m;
    assign rgmii_txd    = 4'd0;
    assign rgmii_tx_ctl = 1'b0;

endmodule
