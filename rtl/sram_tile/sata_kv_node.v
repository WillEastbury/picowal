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
// BOM:
//   ECP5 LFE5U-25F          $10
//   RTL8211F GbE PHY         $2.50
//   M.2 SATA connector       $1
//   Ag9905 PoE PD            $15
//   RJ45 + magnetics         $3
//   Crystal + passives       $5
//   PCB (4-layer, SERDES)    $8
//   ─────────────────────────────
//   Node (without drive):    $44.50
//
//   + 1TB M.2 SATA SSD:     ~$45
//   ═════════════════════════════
//   TOTAL:                   ~$90 ≈ £72
//
//   + 2TB M.2 SATA SSD:     ~$80
//   TOTAL:                   ~$125 ≈ £100
//
//   + 4TB M.2 SATA SSD:     ~$150
//   TOTAL:                   ~$195 ≈ £155
//
// That's a 1TB network KV store for £72. Swap drive for instant upgrade.

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

    // In real design: ECP5 PLL primitive
    // EHXPLLL #(...) pll_inst (...);
    // For simulation, just use input clock
    assign clk_50m  = clk_12m;  // placeholder
    assign clk_125m = clk_12m;  // placeholder
    assign clk_150m = clk_12m;  // placeholder
    assign pll_lock = 1'b1;

    wire rst_n = pll_lock;

    // =====================================================================
    // SATA link layer (ECP5 SERDES → SATA PHY → link → transport)
    // =====================================================================
    //
    // In production: instantiate LiteSATA or custom SATA stack:
    //   SERDES (ECP5 DCU) → OOB signaling → 8b/10b → link init
    //   → FIS transport → command layer
    //
    // Interface to KV engine:
    //   sata_cmd_valid, sata_cmd_ready
    //   sata_cmd_rw     (0=read, 1=write)
    //   sata_cmd_lba    (48-bit LBA)
    //   sata_cmd_count  (sector count, 8 for 4KB)
    //   sata_rd_data, sata_rd_valid
    //   sata_wr_data, sata_wr_ready
    //   sata_cmd_done, sata_cmd_error

    reg         sata_cmd_valid;
    wire        sata_cmd_ready;
    reg         sata_cmd_rw;
    reg  [47:0] sata_cmd_lba;
    reg  [15:0] sata_cmd_count;
    wire [31:0] sata_rd_data;
    wire        sata_rd_valid;
    reg  [31:0] sata_wr_data;
    wire        sata_wr_ready;
    wire        sata_cmd_done;
    wire        sata_cmd_error;
    wire        sata_link_up;

    // Stub for simulation (SATA stack instantiated in synthesis)
    assign sata_cmd_ready = 1'b1;
    assign sata_rd_valid  = 1'b0;
    assign sata_wr_ready  = 1'b1;
    assign sata_cmd_done  = 1'b0;
    assign sata_cmd_error = 1'b0;
    assign sata_link_up   = 1'b1;

    // =====================================================================
    // Ingress FIFO: 72-bit entries [71:64]=flags [63:0]=uint64 address
    // =====================================================================

    reg [71:0] ingress_fifo [0:255];
    reg [7:0]  ingress_wr, ingress_rd;
    wire       ingress_empty = (ingress_wr == ingress_rd);
    wire       ingress_full  = ((ingress_wr + 1) == ingress_rd);

    reg        ingress_push;
    reg [71:0] ingress_din;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n)
            ingress_wr <= 8'd0;
        else if (ingress_push && !ingress_full) begin
            ingress_fifo[ingress_wr] <= ingress_din;
            ingress_wr <= ingress_wr + 1;
        end
    end

    // =====================================================================
    // KV Engine: translate uint64 address → SATA LBA → issue command
    // =====================================================================
    //
    // Address modes:
    //   DIRECT:  LBA = addr[41:0] × SECTORS_PER_PAGE
    //            Supports 2^42 × 4KB = 16 PB virtual (SSD maps internally)
    //   INDEXED: NOR flash hash(uint64) → physical LBA (tenant isolation)
    //
    // For simplicity, start with DIRECT mode (no NOR needed).
    // Tenant/card bits are just part of the LBA calculation.

    localparam KV_IDLE    = 3'd0;
    localparam KV_PARSE   = 3'd1;
    localparam KV_SATA_CMD = 3'd2;
    localparam KV_SATA_RD = 3'd3;
    localparam KV_SATA_WR = 3'd4;
    localparam KV_RESPOND = 3'd5;

    reg [2:0]  kv_state;
    reg [63:0] kv_addr;
    reg        kv_is_write;
    reg [12:0] kv_byte_cnt;

    // Page buffer (4KB in BRAM, dual-port for SATA ↔ network)
    reg [7:0]  page_buf [0:4095];
    reg [11:0] buf_wr_ptr, buf_rd_ptr;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            kv_state      <= KV_IDLE;
            ingress_rd    <= 8'd0;
            sata_cmd_valid <= 1'b0;
            kv_byte_cnt   <= 13'd0;
        end else begin
            sata_cmd_valid <= 1'b0;

            case (kv_state)
                KV_IDLE: begin
                    if (!ingress_empty) begin
                        kv_is_write <= ingress_fifo[ingress_rd][64];
                        kv_addr     <= ingress_fifo[ingress_rd][63:0];
                        ingress_rd  <= ingress_rd + 1;
                        kv_state    <= KV_PARSE;
                    end
                end

                KV_PARSE: begin
                    // Direct LBA: lower 42 bits × 8 sectors
                    sata_cmd_lba   <= {6'd0, kv_addr[41:0]};
                    sata_cmd_count <= SECTORS_PER_PAGE;
                    sata_cmd_rw    <= kv_is_write;
                    sata_cmd_valid <= 1'b1;
                    kv_byte_cnt    <= 13'd0;
                    buf_wr_ptr     <= 12'd0;
                    buf_rd_ptr     <= 12'd0;

                    if (kv_is_write)
                        kv_state <= KV_SATA_WR;
                    else
                        kv_state <= KV_SATA_RD;
                end

                KV_SATA_RD: begin
                    // Collect 4KB from SATA into page buffer
                    if (sata_rd_valid) begin
                        page_buf[buf_wr_ptr]   <= sata_rd_data[7:0];
                        page_buf[buf_wr_ptr+1] <= sata_rd_data[15:8];
                        page_buf[buf_wr_ptr+2] <= sata_rd_data[23:16];
                        page_buf[buf_wr_ptr+3] <= sata_rd_data[31:24];
                        buf_wr_ptr <= buf_wr_ptr + 4;
                    end
                    if (sata_cmd_done)
                        kv_state <= KV_RESPOND;
                end

                KV_SATA_WR: begin
                    // Feed page buffer to SATA
                    if (sata_wr_ready && kv_byte_cnt < PAGE_SIZE) begin
                        sata_wr_data <= {
                            page_buf[buf_rd_ptr+3],
                            page_buf[buf_rd_ptr+2],
                            page_buf[buf_rd_ptr+1],
                            page_buf[buf_rd_ptr]
                        };
                        buf_rd_ptr  <= buf_rd_ptr + 4;
                        kv_byte_cnt <= kv_byte_cnt + 4;
                    end
                    if (sata_cmd_done)
                        kv_state <= KV_RESPOND;
                end

                KV_RESPOND: begin
                    // Page data in buffer → network TX drains it
                    kv_state <= KV_IDLE;
                end

                default: kv_state <= KV_IDLE;
            endcase
        end
    end

    // =====================================================================
    // NOR flash index (optional, active only in INDEXED mode)
    // =====================================================================

    // For DIRECT mode, NOR pins are unused
    assign nor_addr = 22'd0;
    assign nor_dq   = 16'hzzzz;
    assign nor_ce_n = 1'b1;
    assign nor_oe_n = 1'b1;
    assign nor_we_n = 1'b1;

    // =====================================================================
    // Status
    // =====================================================================

    assign led_link     = sata_link_up;
    assign led_sata_rdy = sata_link_up & sata_cmd_ready;
    assign led_activity = ~ingress_empty;
    assign led_error    = sata_cmd_error;

    assign phy_rst_n = rst_n;
    assign mdc       = 1'b0;
    assign mdio      = 1'bz;

    // RGMII stubs (MAC module connects here)
    assign rgmii_txc    = clk_125m;
    assign rgmii_txd    = 4'd0;
    assign rgmii_tx_ctl = 1'b0;

endmodule
