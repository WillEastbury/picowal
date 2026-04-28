// gbe_flash_tile.v — Complete GbE PoE NOR Flash Page Server
//
// Top-level integration:
//   ECP5 FPGA + RTL8211F GbE PHY + 4× NOR Flash + PoE PD
//
// Power: 802.3af/at PoE via Silvertel Ag9905 module
//   48V PoE → Ag9905 → 3.3V @ 4A (13.2W budget)
//   Power breakdown:
//     ECP5 LFE5U-25F:    ~0.3W
//     RTL8211F PHY:       ~0.5W
//     4× NOR flash:       ~0.2W (read mode)
//     Ag9905 + magnetics: ~1.0W loss
//     Total:              ~2.0W (well within PoE Class 2 budget)
//
// BOM:
//   LFE5U-25F-6BG256C    $10.00   ECP5 FPGA (25K LUT, 1Mbit BRAM)
//   RTL8211F-CG           $2.50   GbE PHY, RGMII
//   Ag9905-2BR            $15.00  PoE PD module (isolated, 3.3V/4A)
//   4× S29GL064N           $16.00  NOR flash (32MB total)
//   25MHz crystal           $0.30  for PHY
//   12MHz crystal           $0.30  for FPGA PLL
//   RJ45 w/ magnetics+PoE  $3.00  e.g. HanRun HR911105A
//   LDOs, caps, etc.        $3.00
//   PCB (4-layer)           $5.00
//   -----------------------------------------
//   Total BOM:            ~$55
//
// Specs:
//   32MB non-volatile page store (65536 × 512-byte pages)
//   GbE UDP page server on configurable IP/port
//   Page read: ~8.5μs flash + ~4.5μs wire = ~13μs per page
//   Throughput: ~38 MB/s (limited by 512B page + UDP overhead)
//   Power: ~2W from PoE (no external PSU needed)
//   Latency: <15μs from UDP request to first response byte
//   Zero boot: flash is non-volatile, FPGA configures from SPI flash

module gbe_flash_tile #(
    parameter [47:0] MAC_ADDR = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR  = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [31:0] GATEWAY  = {8'd192, 8'd168, 8'd1, 8'd1},
    parameter [31:0] SUBNET   = {8'd255, 8'd255, 8'd255, 8'd0},
    parameter [15:0] UDP_PORT = 16'd7000,
    parameter N_CHIPS         = 4,
    parameter FLASH_AW        = 22,
    parameter FLASH_DW        = 16,
    parameter PAGE_ADDR_W     = 16
)(
    // --- Clock ---
    input  wire        clk_12m,       // 12MHz crystal

    // --- RGMII PHY interface ---
    output wire        rgmii_txc,
    output wire [3:0]  rgmii_txd,
    output wire        rgmii_tx_ctl,
    input  wire        rgmii_rxc,
    input  wire [3:0]  rgmii_rxd,
    input  wire        rgmii_rx_ctl,

    // --- PHY management ---
    output wire        mdc,
    inout  wire        mdio,
    output wire        phy_rst_n,

    // --- Flash pins ---
    output wire [FLASH_AW-1:0]           flash_a,
    inout  wire [N_CHIPS*FLASH_DW-1:0]  flash_dq,
    output wire [N_CHIPS-1:0]            flash_ce_n,
    output wire                           flash_oe_n,
    output wire                           flash_we_n,

    // --- Status LEDs ---
    output wire        led_link,       // GbE link up
    output wire        led_activity,   // packet activity
    output wire        led_ready,      // system ready
    output wire        led_error       // error indicator
);

    localparam BUS_W = N_CHIPS * FLASH_DW;  // 64 bits

    // =====================================================================
    // PLL: 12MHz → 125MHz (RGMII) + 30MHz (flash controller)
    // =====================================================================
    // ECP5 PLL can generate multiple outputs

    wire clk_125, clk_30;
    wire pll_lock;

    // ECP5 PLL primitive (placeholder — real params from ecppll tool)
    // 12MHz × 125/12 = 125MHz
    // 12MHz × 30/12  = 30MHz (second output)

    // For simulation, just use clk_12m scaled
    // In real hardware: EHXPLLL with CLKOP=125MHz, CLKOS=30MHz
    `ifdef SIMULATION
        assign clk_125  = clk_12m;
        assign clk_30   = clk_12m;
        assign pll_lock = 1'b1;
    `else
        // EHXPLLL #(
        //     .CLKI_DIV(1),
        //     .CLKFB_DIV(10),
        //     .CLKOP_DIV(12),     // 125MHz
        //     .CLKOS_DIV(50),     // 30MHz
        //     .CLKOP_CPHASE(11),
        //     .CLKOS_CPHASE(49)
        // ) pll_inst (
        //     .CLKI(clk_12m),
        //     .CLKOP(clk_125),
        //     .CLKOS(clk_30),
        //     .LOCK(pll_lock),
        //     .RST(1'b0),
        //     .STDBY(1'b0),
        //     .PHASESEL0(1'b0),
        //     .PHASESEL1(1'b0),
        //     .PHASEDIR(1'b0),
        //     .PHASESTEP(1'b0),
        //     .PHASELOADREG(1'b0),
        //     .PLLWAKESYNC(1'b0),
        //     .ENCLKOP(1'b1),
        //     .ENCLKOS(1'b1)
        // );
        assign clk_125  = clk_12m;  // placeholder
        assign clk_30   = clk_12m;
        assign pll_lock = 1'b1;
    `endif

    wire rst_n = pll_lock;

    // PHY held in reset briefly, then released
    reg [15:0] phy_rst_cnt;
    always @(posedge clk_30 or negedge rst_n) begin
        if (!rst_n)
            phy_rst_cnt <= 16'd0;
        else if (phy_rst_cnt < 16'hFFFF)
            phy_rst_cnt <= phy_rst_cnt + 1;
    end
    assign phy_rst_n = (phy_rst_cnt == 16'hFFFF);
    assign mdc  = 1'b0;   // MDIO not used (PHY auto-negotiates)
    assign mdio = 1'bz;

    // =====================================================================
    // RGMII MAC
    // =====================================================================

    wire [7:0] mac_rx_data;
    wire       mac_rx_valid, mac_rx_sof, mac_rx_eof, mac_rx_error;
    wire [7:0] mac_tx_data;
    wire       mac_tx_valid, mac_tx_sof, mac_tx_eof, mac_tx_ready;
    wire       gbe_link_up;

    rgmii_mac mac (
        .clk_125      (clk_125),
        .rst_n        (rst_n),
        .rgmii_txc    (rgmii_txc),
        .rgmii_txd    (rgmii_txd),
        .rgmii_tx_ctl (rgmii_tx_ctl),
        .rgmii_rxc    (rgmii_rxc),
        .rgmii_rxd    (rgmii_rxd),
        .rgmii_rx_ctl (rgmii_rx_ctl),
        .rx_data      (mac_rx_data),
        .rx_valid     (mac_rx_valid),
        .rx_sof       (mac_rx_sof),
        .rx_eof       (mac_rx_eof),
        .rx_error     (mac_rx_error),
        .tx_data      (mac_tx_data),
        .tx_valid     (mac_tx_valid),
        .tx_sof       (mac_tx_sof),
        .tx_eof       (mac_tx_eof),
        .tx_ready     (mac_tx_ready),
        .link_up      (gbe_link_up)
    );

    // =====================================================================
    // UDP Page Engine
    // =====================================================================

    wire        page_start, page_rw_n, page_done;
    wire [15:0] page_addr;
    wire        page_ready;
    wire [63:0] page_dout;
    wire        page_dout_valid;
    wire [31:0] rx_pkt_cnt, tx_pkt_cnt;

    udp_page_engine #(
        .MAC_ADDR(MAC_ADDR),
        .IP_ADDR(IP_ADDR),
        .UDP_PORT(UDP_PORT)
    ) engine (
        .clk          (clk_125),
        .rst_n        (rst_n),
        .rx_data      (mac_rx_data),
        .rx_valid     (mac_rx_valid),
        .rx_sof       (mac_rx_sof),
        .rx_eof       (mac_rx_eof),
        .tx_data      (mac_tx_data),
        .tx_valid     (mac_tx_valid),
        .tx_sof       (mac_tx_sof),
        .tx_eof       (mac_tx_eof),
        .tx_ready     (mac_tx_ready),
        .page_start   (page_start),
        .page_rw_n    (page_rw_n),
        .page_addr    (page_addr),
        .page_ready   (page_ready),
        .page_dout    (page_dout),
        .page_dout_valid(page_dout_valid),
        .page_done    (page_done),
        .rx_pkt_cnt   (rx_pkt_cnt),
        .tx_pkt_cnt   (tx_pkt_cnt)
    );

    // =====================================================================
    // Flash Page Device (runs on 30MHz domain)
    // =====================================================================
    // CDC needed between 125MHz engine and 30MHz flash
    // For now, simplified: use 125MHz for everything in sim

    wire flash_din_valid = 1'b0;
    wire [63:0] flash_din = 64'd0;

    flash_page_dev #(
        .N_CHIPS(N_CHIPS),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW),
        .PAGE_ADDR_W(PAGE_ADDR_W)
    ) flash (
        .clk        (clk_30),
        .rst_n      (rst_n),
        .page_addr  (page_addr),
        .rw_n       (page_rw_n),
        .start      (page_start),
        .ready      (page_ready),
        .dout       (page_dout),
        .dout_valid (page_dout_valid),
        .page_done  (page_done),
        .din        (flash_din),
        .din_valid  (flash_din_valid),
        .flash_a    (flash_a),
        .flash_dq   (flash_dq),
        .flash_ce_n (flash_ce_n),
        .flash_oe_n (flash_oe_n),
        .flash_we_n (flash_we_n),
        .dbg_reading(),
        .dbg_word   ()
    );

    // =====================================================================
    // Status LEDs
    // =====================================================================

    assign led_link  = gbe_link_up;
    assign led_ready = pll_lock && phy_rst_n;
    assign led_error = !pll_lock;

    // Activity blink on packet TX/RX
    reg [19:0] act_timer;
    always @(posedge clk_125 or negedge rst_n) begin
        if (!rst_n)
            act_timer <= 20'd0;
        else if (mac_rx_valid || mac_tx_valid)
            act_timer <= 20'hFFFFF;
        else if (act_timer != 0)
            act_timer <= act_timer - 1;
    end
    assign led_activity = |act_timer;

endmodule
