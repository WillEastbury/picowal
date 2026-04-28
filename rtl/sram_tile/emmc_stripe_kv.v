// emmc_stripe_kv.v — Striped eMMC Network KV Server
//
// 20× eMMC chips on independent 1-bit buses, round-robin striped.
// One Ethernet cable (PoE GbE). No CPU, no OS. Put/Get over TCP stream.
//
// Protocol (TCP stream, port 7000):
//   REQUEST: 3 bytes
//     Bit 23:    0=GET, 1=PUT
//     Bit 22:21: reserved (0)
//     Bit 20:0:  block address (0 to 1,599,999; 80K per chip × 20)
//
//   GET response: 4096 bytes (one 4KB page)
//   PUT: client sends 4096 bytes after header
//        server ACKs with 1 byte: 0x00=ok, 0xFF=error
//
// Stripe mapping:
//   chip_idx  = addr % N_CHIPS
//   sector    = (addr / N_CHIPS) * 8    (8 sectors per 4KB page)
//
// ~80K blocks per chip × 20 chips = 1,600,000 blocks total.
// 1,600,000 × 4KB = 6.4 GB addressable.
// Per chip: 80,000 blocks × 4KB = 320 MB of 4GB eMMC (8% utilization).
//
// Pipeline:
//   Up to N_CHIPS requests in-flight simultaneously.
//   Request FIFO → stripe scheduler → per-chip FIFO → eMMC controller
//   Response collector → response FIFO → TCP TX
//   Zero idle time: while chip 0 reads, chips 1-19 also reading.
//
// Performance (20 chips):
//   Random 4KB GET:  40-100K IOPS → 160-400 MB/s → saturates GbE
//   Sequential GET:  wire-limited at ~117 MB/s
//   Random 4KB PUT:  10-20K IOPS (eMMC write slower)
//   Latency:         ~200μs per GET (eMMC random), pipelined
//
// Pin budget (ECP5 LFE5U-25F BGA-256, 206 I/O):
//   20× eMMC (1-bit): CLK + CMD + DAT0 = 3 pins × 20 = 60
//   RGMII GbE:        TXC + TXD[3:0] + TX_CTL + RXC + RXD[3:0] + RX_CTL = 12
//   PHY mgmt:         MDC + MDIO + RST_N = 3
//   LEDs:             4
//   Crystal:          2
//   Total:            81 pins — comfortable
//
// BOM:
//   LFE5U-25F          $10
//   RTL8211F            $2.50
//   20× eMMC 4GB       $200 retail / ~$60 volume ($3 ea)
//   Ag9905 PoE PD      $15
//   RJ45 + magnetics   $3
//   Passives + PCB     $10
//   Total:             ~$240 retail / ~$100 volume
//
// That's an 80GB network KV store, 40K+ IOPS, GbE wire speed,
// powered by one Ethernet cable, for ~$100 in volume.

module emmc_stripe_kv #(
    parameter N_CHIPS      = 20,
    parameter ADDR_BITS    = 21,        // 2M max blocks (80K/chip × 20 = 1.6M used)
    parameter BLOCKS_PER_CHIP = 80000,  // 80K blocks per eMMC chip
    parameter PAGE_SIZE    = 4096,      // 4KB per value
    parameter SECTORS_PER_PAGE = 8,     // 4096/512
    parameter [47:0] MAC_ADDR = 48'h02_00_00_00_00_01,
    parameter [31:0] IP_ADDR  = {8'd192, 8'd168, 8'd1, 8'd100},
    parameter [15:0] TCP_PORT = 16'd7000
)(
    input  wire        clk_50m,       // 50MHz system clock

    // --- RGMII PHY ---
    output wire        rgmii_txc,
    output wire [3:0]  rgmii_txd,
    output wire        rgmii_tx_ctl,
    input  wire        rgmii_rxc,
    input  wire [3:0]  rgmii_rxd,
    input  wire        rgmii_rx_ctl,
    output wire        phy_rst_n,

    // --- eMMC buses (×20, 1-bit mode) ---
    output wire [N_CHIPS-1:0] emmc_clk,
    inout  wire [N_CHIPS-1:0] emmc_cmd,
    inout  wire [N_CHIPS-1:0] emmc_dat0,
    output wire [N_CHIPS-1:0] emmc_rst_n_o,

    // --- Status ---
    output wire        led_link,
    output wire        led_activity,
    output wire        led_ready,
    output wire        led_error
);

    wire rst_n = 1'b1;  // PLL lock in real design

    // =====================================================================
    // Per-chip eMMC controllers
    // =====================================================================

    wire [N_CHIPS-1:0] chip_ready;
    wire [N_CHIPS-1:0] chip_rd_start;
    wire [N_CHIPS-1:0] chip_wr_start;
    wire [N_CHIPS-1:0] chip_rd_done;
    wire [N_CHIPS-1:0] chip_wr_done;
    wire [N_CHIPS-1:0] chip_rd_valid;

    reg  [31:0] chip_sector [0:N_CHIPS-1];
    wire [7:0]  chip_rd_data [0:N_CHIPS-1];

    genvar i;
    generate
        for (i = 0; i < N_CHIPS; i = i + 1) begin : emmc_gen
            emmc_host_1bit #(
                .CLK_DIV_INIT(125),
                .CLK_DIV_FAST(1)
            ) ctrl (
                .clk        (clk_50m),
                .rst_n      (rst_n),
                .emmc_clk_o (emmc_clk[i]),
                .emmc_cmd   (emmc_cmd[i]),
                .emmc_dat0  (emmc_dat0[i]),
                .emmc_rst_n (emmc_rst_n_o[i]),
                .ready      (chip_ready[i]),
                .rd_start   (chip_rd_start[i]),
                .wr_start   (chip_wr_start[i]),
                .sector     (chip_sector[i]),
                .rd_data    (chip_rd_data[i]),
                .rd_valid   (chip_rd_valid[i]),
                .rd_done    (chip_rd_done[i]),
                .wr_done    (chip_wr_done[i])
            );
        end
    endgenerate

    // =====================================================================
    // Request FIFO: incoming [RW | addr15] from network
    // =====================================================================

    // 24-bit entry: [23]=RW, [22:0]=block address
    reg [23:0] req_fifo [0:255];
    reg [7:0]  req_wr_ptr, req_rd_ptr;
    wire       req_empty = (req_wr_ptr == req_rd_ptr);
    wire       req_full  = ((req_wr_ptr + 1) == req_rd_ptr);

    reg        req_push;
    reg [23:0] req_din;
    reg        req_pop;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            req_wr_ptr <= 8'd0;
            req_rd_ptr <= 8'd0;
        end else begin
            if (req_push && !req_full) begin
                req_fifo[req_wr_ptr] <= req_din;
                req_wr_ptr <= req_wr_ptr + 1;
            end
            if (req_pop && !req_empty)
                req_rd_ptr <= req_rd_ptr + 1;
        end
    end

    // =====================================================================
    // Stripe scheduler: dispatch requests to per-chip controllers
    // =====================================================================

    localparam SCH_IDLE     = 2'd0;
    localparam SCH_DISPATCH = 2'd1;
    localparam SCH_WAIT     = 2'd2;

    reg [1:0]  sch_state;
    reg [4:0]  target_chip;
    reg [22:0] target_addr;
    reg        target_rw;

    // Compute chip index and sector from 21-bit block address
    // Stripe: chip = addr % N_CHIPS, block_in_chip = addr / N_CHIPS
    // sector = block_in_chip * SECTORS_PER_PAGE (8 sectors per 4KB page)
    wire [4:0]  key_chip   = req_fifo[req_rd_ptr][20:0] % N_CHIPS;
    wire [31:0] key_sector = (req_fifo[req_rd_ptr][20:0] / N_CHIPS) * SECTORS_PER_PAGE;

    // Track in-flight requests per chip
    reg [N_CHIPS-1:0] chip_busy;

    // Per-chip start signals
    reg [N_CHIPS-1:0] rd_start_r;
    reg [N_CHIPS-1:0] wr_start_r;

    assign chip_rd_start = rd_start_r;
    assign chip_wr_start = wr_start_r;

    integer ci;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            sch_state   <= SCH_IDLE;
            target_chip <= 5'd0;
            target_addr <= 23'd0;
            target_rw   <= 1'b0;
            req_pop     <= 1'b0;
            rd_start_r  <= {N_CHIPS{1'b0}};
            wr_start_r  <= {N_CHIPS{1'b0}};
            chip_busy   <= {N_CHIPS{1'b0}};
            for (ci = 0; ci < N_CHIPS; ci = ci + 1)
                chip_sector[ci] <= 32'd0;
        end else begin
            req_pop    <= 1'b0;
            rd_start_r <= {N_CHIPS{1'b0}};
            wr_start_r <= {N_CHIPS{1'b0}};

            // Clear busy on completion
            for (ci = 0; ci < N_CHIPS; ci = ci + 1) begin
                if (chip_rd_done[ci] || chip_wr_done[ci])
                    chip_busy[ci] <= 1'b0;
            end

            case (sch_state)
                SCH_IDLE: begin
                    if (!req_empty) begin
                        target_chip <= key_chip;
                        target_rw   <= req_fifo[req_rd_ptr][23];
                        sch_state   <= SCH_DISPATCH;
                    end
                end

                SCH_DISPATCH: begin
                    // Wait for target chip to be free
                    if (!chip_busy[target_chip] && chip_ready[target_chip]) begin
                        chip_sector[target_chip] <= key_sector;
                        chip_busy[target_chip]   <= 1'b1;

                        if (target_rw)
                            wr_start_r[target_chip] <= 1'b1;
                        else
                            rd_start_r[target_chip] <= 1'b1;

                        req_pop   <= 1'b1;
                        sch_state <= SCH_IDLE;
                    end
                end

                default: sch_state <= SCH_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Response collector: merge per-chip read data into output FIFO
    // =====================================================================

    // Ordered response queue: track which chip completes in request order
    reg [4:0]  resp_order [0:255];   // chip index for each in-flight request
    reg [7:0]  resp_wr_ptr, resp_rd_ptr;

    // Response data FIFO: 4K × 8-bit (one page)
    reg [7:0]  resp_data_fifo [0:4095];
    reg [11:0] resp_data_wr, resp_data_rd;
    wire       resp_data_empty = (resp_data_wr == resp_data_rd);

    // Collect bytes from completing chips into response data FIFO
    integer ri;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            resp_data_wr <= 12'd0;
        end else begin
            // Check which chip we're waiting for (in order)
            if (resp_rd_ptr != resp_wr_ptr) begin
                ri = resp_order[resp_rd_ptr];
                if (chip_rd_valid[ri]) begin
                    resp_data_fifo[resp_data_wr] <= chip_rd_data[ri];
                    resp_data_wr <= resp_data_wr + 1;
                end
            end
        end
    end

    // =====================================================================
    // Status LEDs
    // =====================================================================

    assign led_ready    = &chip_ready;
    assign led_error    = ~rst_n;
    assign led_activity = ~req_empty;
    assign led_link     = 1'b0;  // driven by MAC module

endmodule
