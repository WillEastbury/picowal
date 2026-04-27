// hydra_infer_pkg.sv — Shared types and parameters for Hydra INT8 inference engine
// Target: 4x Lattice ECP5UM5G-85F FPGA (per-slice parameters)
// Architecture: 4 FPGA slices in SERDES ring, each with DDR3 + 64-wide systolic
`ifndef HYDRA_INFER_PKG_SV
`define HYDRA_INFER_PKG_SV

package hydra_infer_pkg;

  // ── Platform ───────────────────────────────────────────────
  parameter int NUM_SLICES    = 4;         // 4x ECP5 FPGAs
  parameter int SLICE_ID_W    = 2;         // log2(NUM_SLICES)

  // ── Array geometry (per slice) ─────────────────────────────
  // 64 columns × 4 rows = 256 MACs per ECP5 (128 of 156 DSPs)
  // Total across 4 slices: 1024 MACs
  parameter int ARRAY_COLS    = 64;        // slice width
  parameter int ARRAY_ROWS    = 4;         // slice depth
  parameter int ARRAY_N       = ARRAY_COLS; // backward compat alias
  parameter int MACS_PER_SLICE = ARRAY_COLS * ARRAY_ROWS;  // 256
  parameter int TOTAL_MACS    = MACS_PER_SLICE * NUM_SLICES; // 1024

  // ── ECP5 DSP packing ──────────────────────────────────────
  // ECP5 MULT18X18D: packs 2× INT8 multiply per DSP block
  parameter int INT8_PER_DSP  = 2;
  parameter int DSPS_USED     = MACS_PER_SLICE / INT8_PER_DSP;  // 128

  // ── Data widths ────────────────────────────────────────────
  parameter int DATA_W        = 8;         // INT8 activations & weights
  parameter int ACC_W         = 32;        // INT32 accumulator
  parameter int BIAS_W        = 32;        // INT32 bias
  parameter int SCALE_W       = 16;        // Requant scale (fixed-point)
  parameter int SHIFT_W       = 6;         // Requant right-shift amount

  // ── DDR3 / Wishbone parameters (ECP5 native bus) ───────────
  parameter int WB_ADDR_W     = 28;        // 256MB addressable (512MB chip, x16)
  parameter int WB_DATA_W     = 128;       // 128-bit internal data path
  parameter int WB_SEL_W      = WB_DATA_W / 8;

  // ── SPI slave interface (to RP2354B host) ──────────────────
  parameter int SPI_DATA_W    = 8;
  parameter int SPI_ADDR_W    = 16;        // 64KB register space per FPGA

  // ── SERDES ring parameters ─────────────────────────────────
  parameter int SERDES_LANES  = 4;         // 4 lanes per link
  parameter int SERDES_RATE   = 5_000;     // 5 Gbps per lane
  parameter int RING_PKT_W    = 128;       // inter-slice packet width

  // ── Buffer sizing (per slice) ──────────────────────────────
  parameter int WEIGHT_TILE_BYTES = ARRAY_COLS * ARRAY_ROWS;  // 256B per tile
  parameter int WEIGHT_BUF_DEPTH  = WEIGHT_TILE_BYTES;
  parameter int ACT_BUF_DEPTH     = 1024;
  parameter int PSUM_BUF_DEPTH    = ARRAY_COLS;
  parameter int PARAM_BUF_DEPTH   = 256;

  // ── DMA descriptor ─────────────────────────────────────────
  parameter int DMA_DESC_W    = 96;

  // ── Layer descriptor (microcoded sequencer) ────────────────
  parameter int MAX_LAYERS    = 64;

  // ── Types ──────────────────────────────────────────────────
  typedef logic signed [DATA_W-1:0]   int8_t;
  typedef logic signed [ACC_W-1:0]    int32_t;
  typedef logic signed [2*DATA_W-1:0] int16_t;

  typedef logic [SLICE_ID_W-1:0] slice_id_t;

  typedef struct packed {
    logic [WB_ADDR_W-1:0]   base_addr;
    logic [WB_ADDR_W-1:0]   length;
    logic [WB_ADDR_W-1:0]   config;
  } dma_desc_t;

  typedef struct packed {
    logic [15:0] M;
    logic [15:0] K;
    logic [15:0] N;
    logic [WB_ADDR_W-1:0] weight_addr;
    logic [WB_ADDR_W-1:0] act_addr;
    logic [WB_ADDR_W-1:0] out_addr;
    logic [WB_ADDR_W-1:0] bias_addr;
    logic [SCALE_W-1:0]   requant_scale;
    logic [SHIFT_W-1:0]   requant_shift;
    logic                  relu_en;
    logic                  bias_en;
    logic                  last_layer;
    slice_id_t             target_slice;   // which slice(s) execute this layer
  } layer_desc_t;

  // ── SERDES ring packet ─────────────────────────────────────
  typedef struct packed {
    slice_id_t             src;
    slice_id_t             dst;
    logic [3:0]            pkt_type;       // 0=activation, 1=psum, 2=control
    logic [RING_PKT_W-5:0] payload;
  } ring_pkt_t;

  // ── Performance counter indices ────────────────────────────
  typedef enum logic [3:0] {
    PERF_CYCLES_TOTAL   = 4'd0,
    PERF_CYCLES_ACTIVE  = 4'd1,
    PERF_CYCLES_STALL   = 4'd2,
    PERF_DMA_RD_BEATS   = 4'd3,
    PERF_DMA_WR_BEATS   = 4'd4,
    PERF_WBUF_STALL     = 4'd5,
    PERF_ABUF_STALL     = 4'd6,
    PERF_PSUM_STALL     = 4'd7,
    PERF_LAYERS_DONE    = 4'd8,
    PERF_RING_TX_PKTS   = 4'd9,
    PERF_RING_RX_PKTS   = 4'd10
  } perf_idx_t;
  parameter int NUM_PERF_CTRS = 11;

endpackage
`endif
