// hydra_infer_top.sv — Hydra v7.0 ECP5 inference slice top-level
// One instance per ECP5UM5G-85F. Four slices form the complete engine.
// Interfaces: DDR3 (Wishbone), SPI slave (to RP2354B), SERDES ring

module hydra_infer_top
  import hydra_infer_pkg::*;
#(
  parameter slice_id_t THIS_SLICE = 0,
  parameter int COLS = ARRAY_COLS,  // 64
  parameter int ROWS = ARRAY_ROWS   // 4
)(
  input  logic    clk,           // main fabric clock (~250 MHz)
  input  logic    rst_n,

  // ── DDR3 Wishbone master (to ECP5 DDRPHY + controller) ─────
  output logic                     wb_cyc,
  output logic                     wb_stb,
  output logic                     wb_we,
  output logic [WB_ADDR_W-1:0]   wb_addr,
  output logic [WB_DATA_W-1:0]   wb_dat_o,
  output logic [WB_SEL_W-1:0]    wb_sel,
  input  logic [WB_DATA_W-1:0]   wb_dat_i,
  input  logic                     wb_ack,

  // ── SPI slave (from RP2354B host) ──────────────────────────
  input  logic    spi_sck,
  input  logic    spi_mosi,
  output logic    spi_miso,
  input  logic    spi_cs_n,

  // ── SERDES ring (to adjacent slices) ───────────────────────
  output logic [SERDES_LANES-1:0] ring_tx_p,
  output logic [SERDES_LANES-1:0] ring_tx_n,
  input  logic [SERDES_LANES-1:0] ring_rx_p,
  input  logic [SERDES_LANES-1:0] ring_rx_n,

  // ── Interrupt to RP2354B ───────────────────────────────────
  output logic                     infer_done_irq
);

  // ════════════════════════════════════════════════════════════
  // SPI slave → register bus
  // ════════════════════════════════════════════════════════════

  logic                    spi_reg_wr_en, spi_reg_rd_en;
  logic [SPI_ADDR_W-1:0] spi_reg_addr;
  logic [31:0]            spi_reg_wr_data, spi_reg_rd_data;

  spi_slave u_spi (
    .clk         (clk),
    .rst_n       (rst_n),
    .spi_sck     (spi_sck),
    .spi_mosi    (spi_mosi),
    .spi_miso    (spi_miso),
    .spi_cs_n    (spi_cs_n),
    .reg_wr_en   (spi_reg_wr_en),
    .reg_addr    (spi_reg_addr),
    .reg_wr_data (spi_reg_wr_data),
    .reg_rd_data (spi_reg_rd_data),
    .reg_rd_en   (spi_reg_rd_en)
  );

  // ════════════════════════════════════════════════════════════
  // SERDES ring
  // ════════════════════════════════════════════════════════════

  logic      ring_tx_valid, ring_tx_ready;
  ring_pkt_t ring_tx_pkt;
  logic      ring_rx_valid, ring_rx_ready;
  ring_pkt_t ring_rx_pkt;
  logic      ring_link_up;
  logic [31:0] ring_tx_cnt, ring_rx_cnt;

  serdes_ring #(.THIS_SLICE(THIS_SLICE)) u_ring (
    .clk          (clk),
    .rst_n        (rst_n),
    .tx_data_p    (ring_tx_p),
    .tx_data_n    (ring_tx_n),
    .rx_data_p    (ring_rx_p),
    .rx_data_n    (ring_rx_n),
    .tx_valid     (ring_tx_valid),
    .tx_ready     (ring_tx_ready),
    .tx_pkt       (ring_tx_pkt),
    .rx_valid     (ring_rx_valid),
    .rx_ready     (ring_rx_ready),
    .rx_pkt       (ring_rx_pkt),
    .link_up      (ring_link_up),
    .tx_pkt_count (ring_tx_cnt),
    .rx_pkt_count (ring_rx_cnt)
  );

  assign ring_rx_ready = 1'b1;  // always accept for now
  assign ring_tx_valid = 1'b0;  // driven by layer controller in future
  assign ring_tx_pkt   = '0;

  // ════════════════════════════════════════════════════════════
  // Weight buffer
  // ════════════════════════════════════════════════════════════

  logic                          wbuf_wr_en, wbuf_wr_ready, wbuf_swap, wbuf_fill_done;
  logic [$clog2(COLS*ROWS)-1:0] wbuf_wr_addr;
  int8_t                         wbuf_wr_data;
  logic                          wbuf_rd_req;
  int8_t                         wbuf_tile [COLS][ROWS];
  logic                          wbuf_tile_valid;

  weight_buffer #(.N(COLS*ROWS)) u_wbuf (
    .clk        (clk),
    .rst_n      (rst_n),
    .wr_en      (wbuf_wr_en),
    .wr_addr    (wbuf_wr_addr),
    .wr_data    (wbuf_wr_data),
    .wr_ready   (wbuf_wr_ready),
    .rd_req     (wbuf_rd_req),
    .tile_out   (wbuf_tile),
    .tile_valid (wbuf_tile_valid),
    .swap_banks (wbuf_swap),
    .fill_done  (wbuf_fill_done)
  );

  // ════════════════════════════════════════════════════════════
  // Activation buffer
  // ════════════════════════════════════════════════════════════

  logic  abuf_wr_en, abuf_rd_en, abuf_wr_ready, abuf_rd_valid;
  int8_t abuf_wr_data [ROWS], abuf_rd_data [ROWS];

  activation_buffer #(.N(ROWS)) u_abuf (
    .clk        (clk),
    .rst_n      (rst_n),
    .wr_en      (abuf_wr_en),
    .wr_data    (abuf_wr_data),
    .wr_ready   (abuf_wr_ready),
    .rd_en      (abuf_rd_en),
    .rd_data    (abuf_rd_data),
    .rd_valid   (abuf_rd_valid),
    .fill_level ()
  );

  // ════════════════════════════════════════════════════════════
  // Systolic array (64×4)
  // ════════════════════════════════════════════════════════════

  logic   array_clear, array_load_w, array_valid_in;
  int32_t array_psum_out [COLS];
  logic   array_psum_valid;

  systolic_array #(.COLS(COLS), .ROWS(ROWS)) u_array (
    .clk          (clk),
    .rst_n        (rst_n),
    .clear_acc    (array_clear),
    .load_weights (array_load_w),
    .valid_in     (array_valid_in),
    .weight_in    (wbuf_tile),
    .act_in       (abuf_rd_data),
    .psum_out     (array_psum_out),
    .psum_valid   (array_psum_valid)
  );

  // ════════════════════════════════════════════════════════════
  // Partial sum buffer
  // ════════════════════════════════════════════════════════════

  logic                    psum_wr_en, psum_rd_en, psum_clear_en;
  logic [$clog2(256)-1:0] psum_addr;
  int32_t                  psum_rd_data [COLS];
  logic                    psum_rd_valid;

  psum_buffer #(.N(COLS)) u_psum (
    .clk         (clk),
    .rst_n       (rst_n),
    .wr_en       (psum_wr_en),
    .wr_addr     (psum_addr),
    .wr_data     (array_psum_out),
    .rd_en       (psum_rd_en),
    .rd_addr     (psum_addr),
    .rd_data     (psum_rd_data),
    .rd_valid    (psum_rd_valid),
    .clear_en    (psum_clear_en),
    .clear_addr  (psum_addr)
  );

  // ════════════════════════════════════════════════════════════
  // Activation function
  // ════════════════════════════════════════════════════════════

  logic                actfn_relu_en, actfn_bias_en;
  logic [SCALE_W-1:0] actfn_scale;
  logic [SHIFT_W-1:0] actfn_shift;
  int32_t              actfn_bias [COLS];
  logic                actfn_out_valid;
  int8_t               actfn_out [COLS];

  activation_fn #(.N(COLS)) u_actfn (
    .clk           (clk),
    .rst_n         (rst_n),
    .relu_en       (actfn_relu_en),
    .bias_en       (actfn_bias_en),
    .requant_scale (actfn_scale),
    .requant_shift (actfn_shift),
    .bias_in       (actfn_bias),
    .in_valid      (array_psum_valid),
    .acc_in        (array_psum_out),
    .out_valid     (actfn_out_valid),
    .act_out       (actfn_out)
  );

  // Bias — zeroed for now, loaded via SPI in real impl
  always_comb begin
    for (int i = 0; i < COLS; i++)
      actfn_bias[i] = '0;
  end

  // ════════════════════════════════════════════════════════════
  // Control state (minimal — full layer_controller wiring TBD)
  // ════════════════════════════════════════════════════════════

  logic       ctrl_start, ctrl_done, ctrl_busy;
  logic [5:0] ctrl_layer;
  logic [31:0] perf_active, perf_stall, perf_layers;

  // Tie off control signals — layer controller drives these
  assign array_clear   = 1'b0;
  assign array_load_w  = 1'b0;
  assign array_valid_in = 1'b0;
  assign abuf_rd_en    = 1'b0;
  assign abuf_wr_en    = 1'b0;
  assign wbuf_wr_en    = 1'b0;
  assign wbuf_wr_addr  = '0;
  assign wbuf_wr_data  = '0;
  assign wbuf_rd_req   = 1'b0;
  assign wbuf_swap     = 1'b0;
  assign psum_wr_en    = 1'b0;
  assign psum_rd_en    = 1'b0;
  assign psum_clear_en = 1'b0;
  assign psum_addr     = '0;
  assign actfn_relu_en = 1'b0;
  assign actfn_bias_en = 1'b0;
  assign actfn_scale   = '0;
  assign actfn_shift   = '0;

  assign ctrl_start = 1'b0;  // driven by SPI register write
  assign ctrl_done  = 1'b0;
  assign ctrl_busy  = 1'b0;
  assign ctrl_layer = '0;
  assign perf_active = '0;
  assign perf_stall  = '0;
  assign perf_layers = '0;

  // ════════════════════════════════════════════════════════════
  // DDR3 Wishbone — tie off (DMA engine drives in full impl)
  // ════════════════════════════════════════════════════════════

  assign wb_cyc   = 1'b0;
  assign wb_stb   = 1'b0;
  assign wb_we    = 1'b0;
  assign wb_addr  = '0;
  assign wb_dat_o = '0;
  assign wb_sel   = '0;

  // ════════════════════════════════════════════════════════════
  // SPI register map
  // ════════════════════════════════════════════════════════════
  //
  //   0x0000  CTRL       [0]=start
  //   0x0002  STATUS     [0]=busy, [1]=done, [7:2]=layer
  //   0x0004  SLICE_ID   read-only, returns THIS_SLICE
  //   0x0006  PERF_ACTIVE
  //   0x0008  PERF_STALL
  //   0x000A  PERF_LAYERS
  //   0x000C  RING_STATUS [0]=link_up, [31:16]=tx_cnt[15:0]
  //   0x000E  RING_RX_CNT

  always_comb begin
    spi_reg_rd_data = '0;
    case (spi_reg_addr[7:0])
      8'h00: spi_reg_rd_data = {30'b0, ctrl_done, ctrl_busy};
      8'h02: spi_reg_rd_data = {26'b0, ctrl_layer};
      8'h04: spi_reg_rd_data = {30'b0, THIS_SLICE};
      8'h06: spi_reg_rd_data = perf_active;
      8'h08: spi_reg_rd_data = perf_stall;
      8'h0A: spi_reg_rd_data = perf_layers;
      8'h0C: spi_reg_rd_data = {ring_tx_cnt[15:0], 15'b0, ring_link_up};
      8'h0E: spi_reg_rd_data = ring_rx_cnt;
      default: spi_reg_rd_data = 32'hDEADBEEF;
    endcase
  end

  // ── Interrupt ──────────────────────────────────────────────
  assign infer_done_irq = ctrl_done;

endmodule
