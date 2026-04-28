// emmc_kv_tile.v — eMMC Network KV Server
//
// The simplest possible networked block device:
//   ECP5 FPGA + GbE PHY + eMMC + PoE
//   One Ethernet cable: power + data. Nothing else.
//
// Protocol (TCP stream on configurable port):
//   REQUEST:  2 bytes → [W/R bit | 15-bit page address]
//             Bit 15 = 0: READ,  1: WRITE
//             Bits 14:0 = page address (0-32767)
//   READ:     Server replies with 512 bytes
//   WRITE:    Client sends 512 bytes after header, server ACKs with [0x00]
//
//   Requests are FIFO'd — send many, responses come back in order.
//   No framing, no headers, no JSON. Just raw bytes on a TCP stream.
//
// eMMC interface:
//   8-bit MMC bus (CMD, CLK, DAT[7:0]) at up to 200MHz DDR (HS400)
//   FPGA implements MMC host controller
//   eMMC handles: wear leveling, ECC, bad blocks, garbage collection
//
// Capacity:
//   1× Micron 16GB eMMC: 32M sectors, 15-bit addr uses 32768 × 512 = 16MB
//   To use full capacity: extend address to 25 bits (32M sectors)
//
// Performance:
//   eMMC sequential read:  ~300 MB/s (saturates GbE)
//   eMMC random 4K read:   ~2000-5000 IOPS (~1-2.5 MB/s)
//   eMMC sequential write: ~60-90 MB/s
//   eMMC random 4K write:  ~500-1000 IOPS
//   GbE wire speed:        ~117 MB/s practical
//
// For random KV workloads, consider NOR flash variant (flash_page_fast.v)
// which does ~10M random reads/sec. eMMC wins for bulk/sequential.
//
// BOM:
//   LFE5U-25F-6BG256    $10.00  ECP5 FPGA
//   RTL8211F-CG          $2.50  GbE PHY
//   MTFC16GAKAEEF-AAT   $40.00  16GB eMMC (Micron) — or $5-10 in volume
//   Ag9905-2BR          $15.00  PoE PD module
//   HR911105A            $3.00  RJ45 w/ PoE magnetics
//   25MHz + 12MHz xtals  $0.60
//   Passives + PCB       $8.00
//   Total:              ~$79 (retail) / ~$45 (volume)

module emmc_kv_tile #(
    parameter [47:0] MAC_ADDR  = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR   = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [31:0] GATEWAY   = {8'd192, 8'd168, 8'd1, 8'd1},
    parameter [31:0] SUBNET    = {8'd255, 8'd255, 8'd255, 8'd0},
    parameter [15:0] TCP_PORT  = 16'd7000,
    parameter ADDR_BITS        = 15
)(
    // --- Clock ---
    input  wire        clk_12m,

    // --- RGMII PHY ---
    output wire        rgmii_txc,
    output wire [3:0]  rgmii_txd,
    output wire        rgmii_tx_ctl,
    input  wire        rgmii_rxc,
    input  wire [3:0]  rgmii_rxd,
    input  wire        rgmii_rx_ctl,
    output wire        phy_rst_n,
    output wire        mdc,
    inout  wire        mdio,

    // --- eMMC interface ---
    output wire        emmc_clk,
    inout  wire        emmc_cmd,
    inout  wire [7:0]  emmc_dat,
    output wire        emmc_rst_n,

    // --- Status LEDs ---
    output wire        led_link,
    output wire        led_activity,
    output wire        led_ready,
    output wire        led_error
);

    // =====================================================================
    // PLL: 12MHz → 125MHz (GbE) + 50MHz (eMMC HS) + 200MHz (eMMC HS200)
    // =====================================================================

    wire clk_125, clk_50, clk_200;
    wire pll_lock;

    // ECP5 PLL — placeholder for ecppll-generated parameters
    assign clk_125 = clk_12m;  // placeholder
    assign clk_50  = clk_12m;
    assign clk_200 = clk_12m;
    assign pll_lock = 1'b1;

    wire rst_n = pll_lock;

    // =====================================================================
    // eMMC Host Controller
    // =====================================================================

    // Directly instantiate the eMMC controller submodule
    wire        emmc_ready;
    wire        emmc_rd_start, emmc_wr_start;
    wire [31:0] emmc_sector;
    wire [7:0]  emmc_rd_data;
    wire        emmc_rd_valid;
    wire        emmc_rd_done;
    wire [7:0]  emmc_wr_data;
    wire        emmc_wr_ready;
    wire        emmc_wr_done;

    emmc_host emmc_ctrl (
        .clk         (clk_50),
        .rst_n       (rst_n),
        .emmc_clk_o  (emmc_clk),
        .emmc_cmd    (emmc_cmd),
        .emmc_dat    (emmc_dat),
        .emmc_rst_n  (emmc_rst_n),
        .ready       (emmc_ready),
        .rd_start    (emmc_rd_start),
        .wr_start    (emmc_wr_start),
        .sector      (emmc_sector),
        .rd_data     (emmc_rd_data),
        .rd_valid    (emmc_rd_valid),
        .rd_done     (emmc_rd_done),
        .wr_data     (emmc_wr_data),
        .wr_ready    (emmc_wr_ready),
        .wr_done     (emmc_wr_done)
    );

    // =====================================================================
    // Request FIFO — buffers incoming requests from network
    // =====================================================================
    // Simple sync FIFO in BRAM: 256 entries × 16 bits = 512 bytes

    reg [15:0] req_fifo [0:255];
    reg [7:0]  req_wr_ptr, req_rd_ptr;
    wire       req_fifo_empty = (req_wr_ptr == req_rd_ptr);
    wire       req_fifo_full  = (req_wr_ptr + 1 == req_rd_ptr);

    reg        req_fifo_push;
    reg [15:0] req_fifo_din;
    reg        req_fifo_pop;
    wire [15:0] req_fifo_dout = req_fifo[req_rd_ptr];

    always @(posedge clk_125 or negedge rst_n) begin
        if (!rst_n) begin
            req_wr_ptr <= 8'd0;
            req_rd_ptr <= 8'd0;
        end else begin
            if (req_fifo_push && !req_fifo_full) begin
                req_fifo[req_wr_ptr] <= req_fifo_din;
                req_wr_ptr <= req_wr_ptr + 1;
            end
            if (req_fifo_pop && !req_fifo_empty) begin
                req_rd_ptr <= req_rd_ptr + 1;
            end
        end
    end

    // =====================================================================
    // KV Engine — services requests from FIFO
    // =====================================================================

    localparam KV_IDLE      = 3'd0;
    localparam KV_DISPATCH  = 3'd1;
    localparam KV_READ      = 3'd2;
    localparam KV_READ_SEND = 3'd3;
    localparam KV_WRITE_RX  = 3'd4;
    localparam KV_WRITE_DO  = 3'd5;
    localparam KV_WRITE_ACK = 3'd6;

    reg [2:0]  kv_state;
    reg        kv_rw;               // 0=read, 1=write
    reg [14:0] kv_addr;
    reg [8:0]  kv_byte_cnt;         // 0-511

    // Response FIFO — buffers outgoing data to network
    reg [7:0]  resp_fifo [0:1023];
    reg [9:0]  resp_wr_ptr, resp_rd_ptr;
    wire       resp_fifo_empty = (resp_wr_ptr == resp_rd_ptr);
    reg        resp_push;
    reg [7:0]  resp_din;

    always @(posedge clk_125 or negedge rst_n) begin
        if (!rst_n) begin
            resp_wr_ptr <= 10'd0;
            resp_rd_ptr <= 10'd0;
        end else begin
            if (resp_push) begin
                resp_fifo[resp_wr_ptr] <= resp_din;
                resp_wr_ptr <= resp_wr_ptr + 1;
            end
            // resp_rd_ptr advanced by TX engine
        end
    end

    // Write data buffer — 512 bytes received from network before writing
    reg [7:0]  wr_buf [0:511];
    reg [8:0]  wr_buf_ptr;

    assign emmc_sector   = {17'd0, kv_addr};  // sector = page address
    assign emmc_rd_start = (kv_state == KV_READ) && emmc_ready;
    assign emmc_wr_start = (kv_state == KV_WRITE_DO) && emmc_ready;
    assign emmc_wr_data  = wr_buf[kv_byte_cnt];

    always @(posedge clk_125 or negedge rst_n) begin
        if (!rst_n) begin
            kv_state    <= KV_IDLE;
            kv_rw       <= 1'b0;
            kv_addr     <= 15'd0;
            kv_byte_cnt <= 9'd0;
            wr_buf_ptr  <= 9'd0;
            req_fifo_pop <= 1'b0;
            resp_push   <= 1'b0;
            resp_din    <= 8'd0;
        end else begin
            req_fifo_pop <= 1'b0;
            resp_push    <= 1'b0;

            case (kv_state)
                KV_IDLE: begin
                    if (!req_fifo_empty) begin
                        kv_rw   <= req_fifo_dout[15];
                        kv_addr <= req_fifo_dout[14:0];
                        req_fifo_pop <= 1'b1;
                        kv_state <= KV_DISPATCH;
                    end
                end

                KV_DISPATCH: begin
                    kv_byte_cnt <= 9'd0;
                    if (kv_rw)
                        kv_state <= KV_WRITE_RX;
                    else
                        kv_state <= KV_READ;
                end

                // === READ: start eMMC read, stream bytes to response FIFO ===
                KV_READ: begin
                    if (emmc_ready) begin
                        kv_state <= KV_READ_SEND;
                    end
                end

                KV_READ_SEND: begin
                    if (emmc_rd_valid) begin
                        resp_push <= 1'b1;
                        resp_din  <= emmc_rd_data;
                        kv_byte_cnt <= kv_byte_cnt + 1;
                    end
                    if (emmc_rd_done) begin
                        kv_state <= KV_IDLE;
                    end
                end

                // === WRITE: buffer 512 bytes from network, then write ===
                KV_WRITE_RX: begin
                    // Bytes arrive from TCP RX engine into wr_buf
                    // (connected externally — simplified here)
                    if (wr_buf_ptr >= 9'd512) begin
                        kv_byte_cnt <= 9'd0;
                        kv_state    <= KV_WRITE_DO;
                    end
                end

                KV_WRITE_DO: begin
                    if (emmc_wr_done) begin
                        kv_state <= KV_WRITE_ACK;
                    end
                end

                KV_WRITE_ACK: begin
                    // Send single ACK byte
                    resp_push <= 1'b1;
                    resp_din  <= 8'h00;
                    kv_state  <= KV_IDLE;
                end

                default: kv_state <= KV_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Status
    // =====================================================================

    assign led_ready    = pll_lock && emmc_ready;
    assign led_error    = !pll_lock;
    assign led_link     = 1'b0;  // driven by MAC
    assign led_activity = !req_fifo_empty;

endmodule
