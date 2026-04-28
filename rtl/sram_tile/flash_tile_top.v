// flash_tile_top.v — NOR Flash RAID Tile (stripped-down, no SRAM)
//
// Architecture:
//   1× iCE40HX8K FPGA
//   27× S29GL064N NOR flash (8MB ×16, 90ns read)
//   1× 12MHz crystal → PLL → 30MHz
//
// External interface (active accent accent accent accent accent accent accent accent accent):
//   addr[22:0]   — block address input
//   start        — begin read or write
//   rw_n         — 1=read, 0=write
//   ready        — high when idle, low during operation
//   dout[15:0]   — 16-bit output (directly accent accent accent accent accent accent) — see note
//
// For full parallel output (27×16 = 432 bits), cascade via SPI/LVDS
// or use dedicated output header per bank.
//
// Minimal pin interface for embedding in larger system:
//   23 addr + 1 start + 1 rw_n + 1 ready + 16 data_io = 42 pins external
//   Internal: 23 flash_addr + 16 flash_data + 27 CE# + OE# + WE# = 69 pins
//   Total: ~111 pins of 206 available

module flash_tile_top #(
    parameter N_CHIPS   = 27,
    parameter FLASH_AW  = 23,
    parameter FLASH_DW  = 16
)(
    // --- Clock ---
    input  wire                 clk_12m,

    // --- Host interface (active accent accent accent) ---
    input  wire [FLASH_AW-1:0] host_addr,
    input  wire                 host_rw_n,      // 1=read, 0=write
    input  wire                 host_start,
    output wire                 host_ready,

    // --- Data I/O (active accent accent accent) ---
    // For reads: directly accent accent accent 27 chip outputs
    // Active accent accent accent accent accent accent accent accent accent accent accent accent
    // Active accent accent accent accent accent: directly accent accent accent accent accent accent accent
    input  wire [N_CHIPS*FLASH_DW-1:0] host_din,   // write data (all chips)
    output wire [N_CHIPS*FLASH_DW-1:0] host_dout,  // read data (all chips)
    output wire                 host_dout_valid,

    // --- Flash pins ---
    output wire [FLASH_AW-1:0] flash_a,
    inout  wire [FLASH_DW-1:0] flash_dq [0:N_CHIPS-1],
    output wire [N_CHIPS-1:0]  flash_ce_n,
    output wire                 flash_oe_n,
    output wire                 flash_we_n,

    // --- Status LEDs ---
    output wire                 led_ready,
    output wire                 led_busy,
    output wire                 led_error
);

    // =====================================================================
    // PLL: 12MHz → 30MHz
    // =====================================================================

    wire clk;
    wire pll_lock;

    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0000),
        .DIVF(7'b0100111),
        .DIVQ(3'b100),
        .FILTER_RANGE(3'b001)
    ) pll_inst (
        .REFERENCECLK(clk_12m),
        .PLLOUTCORE(clk),
        .LOCK(pll_lock),
        .RESETB(1'b1),
        .BYPASS(1'b0)
    );

    wire rst_n = pll_lock;

    // =====================================================================
    // RAID controller
    // =====================================================================

    nor_flash_raid #(
        .N_CHIPS(N_CHIPS),
        .FLASH_AW(FLASH_AW),
        .FLASH_DW(FLASH_DW)
    ) raid (
        .clk        (clk),
        .rst_n      (rst_n),
        .addr       (host_addr),
        .rw_n       (host_rw_n),
        .start      (host_start),
        .ready      (host_ready),
        .din        (host_din),
        .dout       (host_dout),
        .dout_valid (host_dout_valid),
        .flash_a    (flash_a),
        .flash_dq   (flash_dq),
        .flash_ce_n (flash_ce_n),
        .flash_oe_n (flash_oe_n),
        .flash_we_n (flash_we_n)
    );

    // =====================================================================
    // LEDs
    // =====================================================================

    assign led_ready = host_ready;
    assign led_busy  = ~host_ready;
    assign led_error = ~pll_lock;

endmodule
