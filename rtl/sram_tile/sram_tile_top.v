// sram_tile_top.v — Complete SRAM lookup tile
//
// Architecture:
//   1× iCE40HX8K FPGA
//   16× IS61WV6416 async SRAM (64K×16, 10ns) on 2 buses
//   1× SD card (SPI) for weight loading
//   1× LVDS link in (from previous tile) — ribbon cable
//   1× LVDS link out (to next tile) — ribbon cable
//   1× 12MHz crystal → PLL → 30MHz system clock
//
// Operation:
//   1. Power on → PLL locks → SD card init
//   2. Load 16 SRAM tables from SD card (2MB total, ~2 seconds @ 15MHz SPI)
//   3. Assert READY, begin accepting pipeline data on LVDS input
//   4. For each input: sweep through 16 stages (32 clocks) → output on LVDS
//
// Pin budget (BGA-256, 206 I/O):
//   Bus A: 16 addr + 16 data + 8 CE# + OE# + WE# = 43
//   Bus B: same                                    = 43
//   SD card: SCLK + MOSI + MISO + CS#             = 4
//   LVDS in:  CLK+/- DATA+/-                       = 4 (2 diff pairs)
//   LVDS out: CLK+/- DATA+/-                       = 4 (2 diff pairs)
//   Crystal: XI, XO                                = 2
//   Status LEDs                                    = 4
//   Total: ~104 pins — comfortable fit

module sram_tile_top #(
    parameter DATA_W     = 16,
    parameter ADDR_W     = 16,
    parameter SRAM_AW    = 16,
    parameter CHIPS_PER_BUS = 8,
    parameter N_CHIPS    = 16
)(
    // --- Clock ---
    input  wire        clk_12m,         // 12MHz crystal

    // --- SRAM Bus A (chips 0-7) ---
    output wire [SRAM_AW-1:0] bus_a_addr,
    inout  wire [DATA_W-1:0]  bus_a_dq,
    output wire [CHIPS_PER_BUS-1:0] bus_a_ce_n,
    output wire               bus_a_oe_n,
    output wire               bus_a_we_n,

    // --- SRAM Bus B (chips 8-15) ---
    output wire [SRAM_AW-1:0] bus_b_addr,
    inout  wire [DATA_W-1:0]  bus_b_dq,
    output wire [CHIPS_PER_BUS-1:0] bus_b_ce_n,
    output wire               bus_b_oe_n,
    output wire               bus_b_we_n,

    // --- SD Card (SPI) ---
    output wire        sd_sclk,
    output wire        sd_mosi,
    input  wire        sd_miso,
    output wire        sd_cs_n,

    // --- LVDS Link In (from previous tile) ---
    input  wire        lvds_rx_clk_p,
    input  wire        lvds_rx_data_p,

    // --- LVDS Link Out (to next tile) ---
    output wire        lvds_tx_clk_p,
    output wire        lvds_tx_data_p,

    // --- Status ---
    output wire        led_ready,       // tables loaded, pipeline active
    output wire        led_busy,        // pipeline sweep in progress
    output wire        led_error,       // SD card or other error
    output wire        led_link          // LVDS link activity
);

    // =====================================================================
    // PLL: 12MHz → 30MHz
    // =====================================================================

    wire clk_30m;
    wire pll_lock;

    // iCE40 PLL primitive (configured via icepll tool)
    // 12MHz × 5 / 2 = 30MHz
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0000),            // DIVR = 0
        .DIVF(7'b0100111),         // DIVF = 39
        .DIVQ(3'b100),             // DIVQ = 4 → /16 → 30MHz
        .FILTER_RANGE(3'b001)
    ) pll_inst (
        .REFERENCECLK(clk_12m),
        .PLLOUTCORE(clk_30m),
        .LOCK(pll_lock),
        .RESETB(1'b1),
        .BYPASS(1'b0)
    );

    wire rst_n = pll_lock;

    // =====================================================================
    // System state machine
    // =====================================================================

    localparam SYS_BOOT    = 2'd0;
    localparam SYS_LOADING = 2'd1;
    localparam SYS_RUN     = 2'd2;
    localparam SYS_ERROR   = 2'd3;

    reg [1:0] sys_state;
    reg       mode_run;
    reg       start_load_r;

    wire      sd_card_ready;
    wire      load_done;
    wire      load_error;
    wire [2:0] sd_error;

    always @(posedge clk_30m or negedge rst_n) begin
        if (!rst_n) begin
            sys_state    <= SYS_BOOT;
            mode_run     <= 1'b0;
            start_load_r <= 1'b0;
        end else begin
            start_load_r <= 1'b0;

            case (sys_state)
                SYS_BOOT: begin
                    if (sd_card_ready) begin
                        sys_state    <= SYS_LOADING;
                        start_load_r <= 1'b1;
                    end
                end

                SYS_LOADING: begin
                    if (load_done) begin
                        sys_state <= SYS_RUN;
                        mode_run  <= 1'b1;
                    end
                    if (load_error || |sd_error) begin
                        sys_state <= SYS_ERROR;
                    end
                end

                SYS_RUN: begin
                    // Normal operation
                end

                SYS_ERROR: begin
                    // Stuck until power cycle
                end
            endcase
        end
    end

    // =====================================================================
    // SD Card Reader
    // =====================================================================

    wire       sd_data_valid;
    wire [7:0] sd_data_byte;
    wire       sd_read_done;
    wire       sd_read_cmd;
    wire [31:0] sd_sector;

    spi_sd_reader #(
        .CLK_DIV_INIT(64),     // 30MHz/64 ≈ 469kHz
        .CLK_DIV_FAST(2)       // 30MHz/2  = 15MHz
    ) sd_reader (
        .clk        (clk_30m),
        .rst_n      (rst_n),
        .cmd_read   (sd_read_cmd),
        .cmd_sector (sd_sector),
        .data_valid (sd_data_valid),
        .data_byte  (sd_data_byte),
        .read_done  (sd_read_done),
        .card_ready (sd_card_ready),
        .error      (sd_error),
        .sd_sclk    (sd_sclk),
        .sd_mosi    (sd_mosi),
        .sd_miso    (sd_miso),
        .sd_cs_n    (sd_cs_n)
    );

    // =====================================================================
    // SRAM Loader
    // =====================================================================

    wire [2:0]        ldr_chip_sel;
    wire              ldr_bus_sel;
    wire [ADDR_W-1:0] ldr_addr;
    wire [DATA_W-1:0] ldr_data;
    wire              ldr_we;

    sram_loader #(
        .N_CHIPS(N_CHIPS),
        .ADDR_W(ADDR_W),
        .DATA_W(DATA_W)
    ) loader (
        .clk           (clk_30m),
        .rst_n         (rst_n),
        .start_load    (start_load_r),
        .load_done     (load_done),
        .load_error    (load_error),
        .load_chip_idx (),
        .sd_read_cmd   (sd_read_cmd),
        .sd_sector     (sd_sector),
        .sd_data_valid (sd_data_valid),
        .sd_data_byte  (sd_data_byte),
        .sd_read_done  (sd_read_done),
        .sd_card_ready (sd_card_ready),
        .sram_chip_sel (ldr_chip_sel),
        .sram_bus_sel  (ldr_bus_sel),
        .sram_addr     (ldr_addr),
        .sram_data     (ldr_data),
        .sram_we       (ldr_we)
    );

    // =====================================================================
    // SRAM Bus Controllers
    // =====================================================================

    // Pipeline data flow: LVDS RX → Bus A (stages 0-7) → Bus B (stages 8-15) → LVDS TX

    wire              bus_a_pipe_valid_out;
    wire [DATA_W-1:0] bus_a_pipe_data_out;
    wire              bus_a_busy;

    wire              bus_b_pipe_valid_out;
    wire [DATA_W-1:0] bus_b_pipe_data_out;
    wire              bus_b_busy;

    // Pipeline input from LVDS RX
    wire              pipe_in_valid;
    wire [DATA_W-1:0] pipe_in_data;

    sram_bus_ctrl #(
        .DATA_W(DATA_W),
        .ADDR_W(ADDR_W),
        .SRAM_AW(SRAM_AW),
        .N_CHIPS(CHIPS_PER_BUS),
        .BUS_ID(0)
    ) bus_a_ctrl (
        .clk            (clk_30m),
        .rst_n          (rst_n),
        .pipe_valid_in  (pipe_in_valid),
        .pipe_data_in   (pipe_in_data),
        .pipe_valid_out (bus_a_pipe_valid_out),
        .pipe_data_out  (bus_a_pipe_data_out),
        .mode_run       (mode_run),
        .load_chip_sel  (ldr_chip_sel),
        .load_addr      (ldr_addr),
        .load_data      (ldr_data),
        .load_we        (ldr_we && !ldr_bus_sel),
        .sram_a         (bus_a_addr),
        .sram_dq        (bus_a_dq),
        .sram_ce_n      (bus_a_ce_n),
        .sram_oe_n      (bus_a_oe_n),
        .sram_we_n      (bus_a_we_n),
        .busy           (bus_a_busy),
        .dbg_stage      ()
    );

    sram_bus_ctrl #(
        .DATA_W(DATA_W),
        .ADDR_W(ADDR_W),
        .SRAM_AW(SRAM_AW),
        .N_CHIPS(CHIPS_PER_BUS),
        .BUS_ID(1)
    ) bus_b_ctrl (
        .clk            (clk_30m),
        .rst_n          (rst_n),
        .pipe_valid_in  (bus_a_pipe_valid_out),
        .pipe_data_in   (bus_a_pipe_data_out),
        .pipe_valid_out (bus_b_pipe_valid_out),
        .pipe_data_out  (bus_b_pipe_data_out),
        .mode_run       (mode_run),
        .load_chip_sel  (ldr_chip_sel),
        .load_addr      (ldr_addr),
        .load_data      (ldr_data),
        .load_we        (ldr_we && ldr_bus_sel),
        .sram_a         (bus_b_addr),
        .sram_dq        (bus_b_dq),
        .sram_ce_n      (bus_b_ce_n),
        .sram_oe_n      (bus_b_oe_n),
        .sram_we_n      (bus_b_we_n),
        .busy           (bus_b_busy),
        .dbg_stage      ()
    );

    // =====================================================================
    // LVDS Tile Links
    // =====================================================================

    lvds_tile_link #(.DATA_W(DATA_W)) link_in (
        .clk           (clk_30m),
        .rst_n         (rst_n),
        .tx_valid      (1'b0),          // TX unused on input link
        .tx_data       ({DATA_W{1'b0}}),
        .tx_ready      (),
        .lvds_tx_clk   (),
        .lvds_tx_data  (),
        .lvds_rx_clk   (lvds_rx_clk_p),
        .lvds_rx_data  (lvds_rx_data_p),
        .rx_valid      (pipe_in_valid),
        .rx_data       (pipe_in_data)
    );

    lvds_tile_link #(.DATA_W(DATA_W)) link_out (
        .clk           (clk_30m),
        .rst_n         (rst_n),
        .tx_valid      (bus_b_pipe_valid_out),
        .tx_data       (bus_b_pipe_data_out),
        .tx_ready      (),
        .lvds_tx_clk   (lvds_tx_clk_p),
        .lvds_tx_data  (lvds_tx_data_p),
        .lvds_rx_clk   (1'b0),         // RX unused on output link
        .lvds_rx_data  (1'b0),
        .rx_valid      (),
        .rx_data       ()
    );

    // =====================================================================
    // Status LEDs
    // =====================================================================

    assign led_ready = (sys_state == SYS_RUN);
    assign led_busy  = bus_a_busy || bus_b_busy;
    assign led_error = (sys_state == SYS_ERROR);

    // Blink on LVDS activity
    reg [19:0] link_blink;
    always @(posedge clk_30m or negedge rst_n) begin
        if (!rst_n)
            link_blink <= 20'd0;
        else if (pipe_in_valid || bus_b_pipe_valid_out)
            link_blink <= 20'hFFFFF;
        else if (link_blink != 0)
            link_blink <= link_blink - 1;
    end
    assign led_link = |link_blink;

endmodule
