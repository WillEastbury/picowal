// layer_controller.sv — Microcoded inference sequencer
// Sequences GEMM tiling: iterates M-tiles × K-tiles × N-tiles
// Coordinates DMA, weight buffer, activation buffer, array, and output

module layer_controller
  import hydra_infer_pkg::*;
#(
  parameter int N = ARRAY_N
)(
  input  logic    clk,
  input  logic    rst_n,

  // ── Control interface (from top-level AXI-Lite) ────────────
  input  logic                     start,         // begin inference
  output logic                     done,          // all layers complete
  output logic                     busy,

  // ── Layer descriptor memory ────────────────────────────────
  input  logic                     desc_wr_en,
  input  logic [$clog2(MAX_LAYERS)-1:0] desc_wr_addr,
  input  layer_desc_t              desc_wr_data,
  output logic [5:0]               current_layer,

  // ── DMA control ────────────────────────────────────────────
  output logic                     dma_desc_valid,
  input  logic                     dma_desc_ready,
  output logic [AXI_ADDR_W-1:0]   dma_addr,
  output logic [AXI_ADDR_W-1:0]   dma_length,
  output logic                     dma_is_write,
  input  logic                     dma_done,

  // ── Weight buffer control ──────────────────────────────────
  output logic                     wbuf_swap,
  input  logic                     wbuf_fill_done,

  // ── Systolic array control ─────────────────────────────────
  output logic                     array_clear_acc,
  output logic                     array_load_weights,
  output logic                     array_valid_in,

  // ── Activation buffer control ──────────────────────────────
  output logic                     abuf_rd_en,
  input  logic                     abuf_rd_valid,

  // ── Partial sum buffer control ─────────────────────────────
  output logic                     psum_wr_en,
  output logic                     psum_rd_en,
  output logic                     psum_clear_en,
  output logic [$clog2(256)-1:0]  psum_addr,

  // ── Activation function control ────────────────────────────
  output logic                     actfn_relu_en,
  output logic                     actfn_bias_en,
  output logic [SCALE_W-1:0]      actfn_scale,
  output logic [SHIFT_W-1:0]      actfn_shift,

  // ── Output writeback ───────────────────────────────────────
  output logic                     out_wr_en,

  // ── Performance counters ───────────────────────────────────
  output logic [31:0]              perf_cycles_active,
  output logic [31:0]              perf_cycles_stall,
  output logic [31:0]              perf_layers_done
);

  // ── Layer descriptor memory ────────────────────────────────
  layer_desc_t desc_mem [MAX_LAYERS];
  layer_desc_t cur_desc;

  always_ff @(posedge clk) begin
    if (desc_wr_en)
      desc_mem[desc_wr_addr] <= desc_wr_data;
  end

  // ── Tiling state ───────────────────────────────────────────
  logic [15:0] M_tiles, K_tiles, N_tiles;  // tile counts
  logic [15:0] m_tile, k_tile, n_tile;     // current tile indices

  // Compute tile counts from layer dimensions
  function automatic logic [15:0] div_ceil(input logic [15:0] a, input logic [15:0] b);
    return (a + b - 1) / b;
  endfunction

  // ── Microcode FSM ──────────────────────────────────────────
  typedef enum logic [4:0] {
    MC_IDLE,
    MC_LOAD_DESC,
    MC_TILE_START,
    MC_DMA_WEIGHT_REQ,
    MC_DMA_WEIGHT_WAIT,
    MC_DMA_ACT_REQ,
    MC_DMA_ACT_WAIT,
    MC_SWAP_WBUF,
    MC_CLEAR_PSUM,
    MC_COMPUTE,
    MC_DRAIN_PSUM,
    MC_STORE_PSUM,
    MC_LAST_K_CHECK,
    MC_ACTIVATE,
    MC_DMA_OUT_REQ,
    MC_DMA_OUT_WAIT,
    MC_TILE_ADVANCE,
    MC_LAYER_ADVANCE,
    MC_DONE
  } mc_state_t;

  mc_state_t mc_state, mc_next;

  logic [15:0] compute_count;  // cycles within a tile compute phase

  // ── State register ─────────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      mc_state <= MC_IDLE;
    else
      mc_state <= mc_next;
  end

  // ── Next state logic ───────────────────────────────────────
  always_comb begin
    mc_next = mc_state;
    case (mc_state)
      MC_IDLE:
        if (start) mc_next = MC_LOAD_DESC;

      MC_LOAD_DESC:
        mc_next = MC_TILE_START;

      MC_TILE_START:
        mc_next = MC_DMA_WEIGHT_REQ;

      MC_DMA_WEIGHT_REQ:
        if (dma_desc_ready) mc_next = MC_DMA_WEIGHT_WAIT;

      MC_DMA_WEIGHT_WAIT:
        if (dma_done) mc_next = MC_DMA_ACT_REQ;

      MC_DMA_ACT_REQ:
        if (dma_desc_ready) mc_next = MC_DMA_ACT_WAIT;

      MC_DMA_ACT_WAIT:
        if (dma_done) mc_next = MC_SWAP_WBUF;

      MC_SWAP_WBUF:
        mc_next = (k_tile == 0) ? MC_CLEAR_PSUM : MC_COMPUTE;

      MC_CLEAR_PSUM:
        mc_next = MC_COMPUTE;

      MC_COMPUTE:
        if (compute_count == N - 1) mc_next = MC_DRAIN_PSUM;

      MC_DRAIN_PSUM:
        mc_next = MC_STORE_PSUM;

      MC_STORE_PSUM:
        mc_next = MC_LAST_K_CHECK;

      MC_LAST_K_CHECK:
        if (k_tile == K_tiles - 1)
          mc_next = MC_ACTIVATE;
        else
          mc_next = MC_TILE_ADVANCE;

      MC_ACTIVATE:
        mc_next = MC_DMA_OUT_REQ;

      MC_DMA_OUT_REQ:
        if (dma_desc_ready) mc_next = MC_DMA_OUT_WAIT;

      MC_DMA_OUT_WAIT:
        if (dma_done) mc_next = MC_TILE_ADVANCE;

      MC_TILE_ADVANCE:
        mc_next = MC_TILE_START;

      MC_LAYER_ADVANCE:
        if (cur_desc.last_layer)
          mc_next = MC_DONE;
        else
          mc_next = MC_LOAD_DESC;

      MC_DONE:
        mc_next = MC_IDLE;

      default: mc_next = MC_IDLE;
    endcase
  end

  // ── Datapath control ───────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      current_layer    <= '0;
      m_tile           <= '0;
      k_tile           <= '0;
      n_tile           <= '0;
      M_tiles          <= '0;
      K_tiles          <= '0;
      N_tiles          <= '0;
      compute_count    <= '0;
      perf_cycles_active <= '0;
      perf_cycles_stall  <= '0;
      perf_layers_done   <= '0;
    end else begin
      case (mc_state)
        MC_LOAD_DESC: begin
          cur_desc  <= desc_mem[current_layer];
          M_tiles   <= div_ceil(desc_mem[current_layer].M, N[15:0]);
          K_tiles   <= div_ceil(desc_mem[current_layer].K, N[15:0]);
          N_tiles   <= div_ceil(desc_mem[current_layer].N, N[15:0]);
          m_tile    <= '0;
          k_tile    <= '0;
          n_tile    <= '0;
        end

        MC_TILE_START: begin
          compute_count <= '0;
        end

        MC_COMPUTE: begin
          compute_count      <= compute_count + 1;
          perf_cycles_active <= perf_cycles_active + 1;
        end

        MC_DMA_WEIGHT_WAIT,
        MC_DMA_ACT_WAIT,
        MC_DMA_OUT_WAIT: begin
          perf_cycles_stall <= perf_cycles_stall + 1;
        end

        MC_TILE_ADVANCE: begin
          // Advance K, then N, then M
          if (k_tile < K_tiles - 1) begin
            k_tile <= k_tile + 1;
          end else begin
            k_tile <= '0;
            if (n_tile < N_tiles - 1) begin
              n_tile <= n_tile + 1;
            end else begin
              n_tile <= '0;
              if (m_tile < M_tiles - 1) begin
                m_tile <= m_tile + 1;
              end else begin
                // All tiles done for this layer
                perf_layers_done <= perf_layers_done + 1;
                current_layer    <= current_layer + 1;
              end
            end
          end
        end

        default: ;
      endcase
    end
  end

  // ── Output assignments ─────────────────────────────────────
  assign busy = (mc_state != MC_IDLE);
  assign done = (mc_state == MC_DONE);

  // DMA control
  always_comb begin
    dma_desc_valid = 1'b0;
    dma_addr       = '0;
    dma_length     = '0;
    dma_is_write   = 1'b0;
    case (mc_state)
      MC_DMA_WEIGHT_REQ: begin
        dma_desc_valid = 1'b1;
        dma_addr       = cur_desc.weight_addr +
                         (n_tile * K_tiles + k_tile) * N * N;
        dma_length     = N * N;
      end
      MC_DMA_ACT_REQ: begin
        dma_desc_valid = 1'b1;
        dma_addr       = cur_desc.act_addr +
                         (m_tile * K_tiles + k_tile) * N * N;
        dma_length     = N * N;
      end
      MC_DMA_OUT_REQ: begin
        dma_desc_valid = 1'b1;
        dma_is_write   = 1'b1;
        dma_addr       = cur_desc.out_addr +
                         (m_tile * N_tiles + n_tile) * N * N;
        dma_length     = N * N;
      end
      default: ;
    endcase
  end

  // Weight buffer
  assign wbuf_swap = (mc_state == MC_SWAP_WBUF);

  // Array
  assign array_clear_acc   = (mc_state == MC_CLEAR_PSUM);
  assign array_load_weights = (mc_state == MC_SWAP_WBUF);
  assign array_valid_in    = (mc_state == MC_COMPUTE) && abuf_rd_valid;
  assign abuf_rd_en        = (mc_state == MC_COMPUTE);

  // Psum buffer
  assign psum_clear_en = (mc_state == MC_CLEAR_PSUM);
  assign psum_addr     = m_tile[$clog2(256)-1:0];
  assign psum_wr_en    = (mc_state == MC_STORE_PSUM);
  assign psum_rd_en    = (mc_state == MC_COMPUTE) && (k_tile != 0);

  // Activation function
  assign actfn_relu_en = cur_desc.relu_en;
  assign actfn_bias_en = cur_desc.bias_en;
  assign actfn_scale   = cur_desc.requant_scale;
  assign actfn_shift   = cur_desc.requant_shift;

  // Output
  assign out_wr_en = (mc_state == MC_ACTIVATE);

endmodule
