// picowal_hx_top.v -- PicoWAL on Alchitry Cu (iCE40HX8K)
// Auto-generated module outline. Implement each submodule separately.

module picowal_hx_top (
    input  wire        clk_100mhz,    // Cu onboard 100MHz oscillator

    // SRAM (Header A + B)
    output wire [17:0] sram_addr,
    inout  wire [15:0] sram_data,
    output wire        sram_ce_n,
    output wire        sram_oe_n,
    output wire        sram_we_n,
    output wire        sram_lb_n,
    output wire        sram_ub_n,

    // W5100S SPI (Header C)
    output wire        w5100_mosi,
    input  wire        w5100_miso,
    output wire        w5100_sck,
    output wire        w5100_cs_n,
    input  wire        w5100_int_n,
    output wire        w5100_rst_n,

    // SD Card SPI (Header C)
    output wire        sd_mosi,
    input  wire        sd_miso,
    output wire        sd_sck,
    output wire        sd_cs_n,
    input  wire        sd_detect,

    // Onboard LEDs (active high on Cu)
    output wire [7:0]  leds
);

    // ─── Clock generation ───────────────────────────────────────────
    wire clk_48;        // 48MHz system clock (PLL from 100MHz)
    wire pll_locked;

    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0100),         // ref div = 4+1 = 5 -> 20MHz
        .DIVF(7'b0010011),      // fb div = 19+1 = 20 -> 20*20=400MHz VCO
        .DIVQ(3'b011),          // out div = 2^3 = 8 -> 50MHz (close to 48)
        .FILTER_RANGE(3'b010)
    ) pll_inst (
        .REFERENCECLK(clk_100mhz),
        .PLLOUTCORE(clk_48),
        .LOCK(pll_locked),
        .RESETB(1'b1),
        .BYPASS(1'b0)
    );

    wire rst_n = pll_locked;

    // ─── PicoScript execution engine ────────────────────────────────
    // 4 connection contexts, round-robin scheduling
    wire [1:0]  ctx_id;          // active context (0-3)
    wire [15:0] pc;              // program counter
    wire [31:0] instruction;     // current instruction word
    wire [3:0]  opcode;          // decoded opcode [31:28]
    wire [3:0]  rd, rs1, rs2;    // register indices
    wire [15:0] imm16;           // immediate value

    // Submodule instances (implement each as separate .v file)
    // picoscript_decode  decode_inst  (.clk(clk_48), ...);
    // picoscript_alu     alu_inst     (.clk(clk_48), ...);
    // picoscript_branch  branch_inst  (.clk(clk_48), ...);
    // picoscript_dsp     dsp_inst     (.clk(clk_48), ...);  // soft MAC
    // sram_controller    sram_inst    (.clk(clk_48), ...);
    // spi_master         w5100_spi    (.clk(clk_48), ...);
    // spi_master         sd_spi       (.clk(clk_48), ...);
    // card_mapper        mapper_inst  (.clk(clk_48), ...);
    // pipe_engine        pipe_inst    (.clk(clk_48), ...);
    // irq_controller     irq_inst     (.clk(clk_48), ...);
    // context_scheduler  sched_inst   (.clk(clk_48), ...);

    assign leds = {pll_locked, ctx_id, opcode[3:0], 1'b0};

endmodule
