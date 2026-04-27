// dma_engine.sv — AXI4 DMA engine for LPDDR4 weight/activation transfers
// Descriptor-based: layer controller programs source, dest, length
// Supports burst reads (weight/activation fetch) and burst writes (output writeback)

module dma_engine
  import hydra_infer_pkg::*;
(
  input  logic    clk,
  input  logic    rst_n,

  // ── Descriptor interface (from layer controller) ───────────
  input  logic                     desc_valid,
  output logic                     desc_ready,
  input  logic [AXI_ADDR_W-1:0]   desc_addr,
  input  logic [AXI_ADDR_W-1:0]   desc_length,   // bytes
  input  logic                     desc_is_write, // 0=read, 1=write

  // ── Data interface to internal buffers ─────────────────────
  // Read channel: DMA reads from LPDDR4, pushes to buffer
  output logic                     data_out_valid,
  input  logic                     data_out_ready,
  output logic [AXI_DATA_W-1:0]   data_out,

  // Write channel: DMA pulls from buffer, writes to LPDDR4
  input  logic                     data_in_valid,
  output logic                     data_in_ready,
  input  logic [AXI_DATA_W-1:0]   data_in,

  // ── Transfer status ────────────────────────────────────────
  output logic                     busy,
  output logic                     done,
  output logic [31:0]              beat_count,    // perf counter

  // ── AXI4 Master interface ──────────────────────────────────
  // Write address channel
  output logic [AXI_ID_W-1:0]     m_axi_awid,
  output logic [AXI_ADDR_W-1:0]   m_axi_awaddr,
  output logic [AXI_LEN_W-1:0]    m_axi_awlen,
  output logic [2:0]               m_axi_awsize,
  output logic [1:0]               m_axi_awburst,
  output logic                     m_axi_awvalid,
  input  logic                     m_axi_awready,

  // Write data channel
  output logic [AXI_DATA_W-1:0]   m_axi_wdata,
  output logic [AXI_STRB_W-1:0]   m_axi_wstrb,
  output logic                     m_axi_wlast,
  output logic                     m_axi_wvalid,
  input  logic                     m_axi_wready,

  // Write response channel
  input  logic [AXI_ID_W-1:0]     m_axi_bid,
  input  logic [1:0]              m_axi_bresp,
  input  logic                     m_axi_bvalid,
  output logic                     m_axi_bready,

  // Read address channel
  output logic [AXI_ID_W-1:0]     m_axi_arid,
  output logic [AXI_ADDR_W-1:0]   m_axi_araddr,
  output logic [AXI_LEN_W-1:0]    m_axi_arlen,
  output logic [2:0]               m_axi_arsize,
  output logic [1:0]               m_axi_arburst,
  output logic                     m_axi_arvalid,
  input  logic                     m_axi_arready,

  // Read data channel
  input  logic [AXI_ID_W-1:0]     m_axi_rid,
  input  logic [AXI_DATA_W-1:0]   m_axi_rdata,
  input  logic [1:0]              m_axi_rresp,
  input  logic                     m_axi_rlast,
  input  logic                     m_axi_rvalid,
  output logic                     m_axi_rready
);

  // ── Constants ──────────────────────────────────────────────
  localparam int BYTES_PER_BEAT = AXI_DATA_W / 8;
  localparam int MAX_BURST_LEN  = 256;  // AXI4 max

  // ── State machine ──────────────────────────────────────────
  typedef enum logic [2:0] {
    S_IDLE,
    S_RD_ADDR,
    S_RD_DATA,
    S_WR_ADDR,
    S_WR_DATA,
    S_WR_RESP,
    S_DONE
  } state_t;

  state_t state, state_next;

  // ── Registers ──────────────────────────────────────────────
  logic [AXI_ADDR_W-1:0]  cur_addr;
  logic [AXI_ADDR_W-1:0]  bytes_remaining;
  logic [AXI_LEN_W-1:0]   burst_beats;
  logic [AXI_LEN_W-1:0]   beat_idx;
  logic                    is_write_r;
  logic [31:0]             beat_count_r;

  assign busy       = (state != S_IDLE);
  assign done       = (state == S_DONE);
  assign beat_count = beat_count_r;

  // ── Burst length calculation ───────────────────────────────
  function automatic logic [AXI_LEN_W-1:0] calc_burst_len(
    input logic [AXI_ADDR_W-1:0] remaining
  );
    logic [AXI_ADDR_W-1:0] beats;
    beats = (remaining + BYTES_PER_BEAT - 1) / BYTES_PER_BEAT;
    if (beats > MAX_BURST_LEN)
      return AXI_LEN_W'(MAX_BURST_LEN - 1);
    else
      return AXI_LEN_W'(beats - 1);
  endfunction

  // ── FSM: next state ────────────────────────────────────────
  always_comb begin
    state_next = state;
    case (state)
      S_IDLE:     if (desc_valid)
                    state_next = desc_is_write ? S_WR_ADDR : S_RD_ADDR;
      S_RD_ADDR:  if (m_axi_arready)
                    state_next = S_RD_DATA;
      S_RD_DATA:  if (m_axi_rvalid && m_axi_rlast) begin
                    if (bytes_remaining <= (burst_beats + 1) * BYTES_PER_BEAT)
                      state_next = S_DONE;
                    else
                      state_next = S_RD_ADDR;
                  end
      S_WR_ADDR:  if (m_axi_awready)
                    state_next = S_WR_DATA;
      S_WR_DATA:  if (m_axi_wready && m_axi_wlast)
                    state_next = S_WR_RESP;
      S_WR_RESP:  if (m_axi_bvalid) begin
                    if (bytes_remaining <= (burst_beats + 1) * BYTES_PER_BEAT)
                      state_next = S_DONE;
                    else
                      state_next = S_WR_ADDR;
                  end
      S_DONE:     state_next = S_IDLE;
      default:    state_next = S_IDLE;
    endcase
  end

  // ── FSM: state register ────────────────────────────────────
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state           <= S_IDLE;
      cur_addr        <= '0;
      bytes_remaining <= '0;
      burst_beats     <= '0;
      beat_idx        <= '0;
      is_write_r      <= 1'b0;
      beat_count_r    <= '0;
    end else begin
      state <= state_next;
      case (state)
        S_IDLE: begin
          if (desc_valid) begin
            cur_addr        <= desc_addr;
            bytes_remaining <= desc_length;
            is_write_r      <= desc_is_write;
            burst_beats     <= calc_burst_len(desc_length);
            beat_idx        <= '0;
          end
        end
        S_RD_ADDR: begin
          if (m_axi_arready)
            beat_idx <= '0;
        end
        S_RD_DATA: begin
          if (m_axi_rvalid && data_out_ready) begin
            beat_idx     <= beat_idx + 1;
            beat_count_r <= beat_count_r + 1;
            if (m_axi_rlast) begin
              cur_addr        <= cur_addr + (burst_beats + 1) * BYTES_PER_BEAT;
              bytes_remaining <= bytes_remaining - (burst_beats + 1) * BYTES_PER_BEAT;
              burst_beats     <= calc_burst_len(
                bytes_remaining - (burst_beats + 1) * BYTES_PER_BEAT
              );
            end
          end
        end
        S_WR_ADDR: begin
          if (m_axi_awready)
            beat_idx <= '0;
        end
        S_WR_DATA: begin
          if (m_axi_wready && data_in_valid) begin
            beat_idx     <= beat_idx + 1;
            beat_count_r <= beat_count_r + 1;
            if (beat_idx == burst_beats) begin
              cur_addr        <= cur_addr + (burst_beats + 1) * BYTES_PER_BEAT;
              bytes_remaining <= bytes_remaining - (burst_beats + 1) * BYTES_PER_BEAT;
              burst_beats     <= calc_burst_len(
                bytes_remaining - (burst_beats + 1) * BYTES_PER_BEAT
              );
            end
          end
        end
        S_DONE: begin
          beat_count_r <= '0;
        end
        default: ;
      endcase
    end
  end

  // ── Descriptor handshake ───────────────────────────────────
  assign desc_ready = (state == S_IDLE);

  // ── AXI Read address channel ───────────────────────────────
  assign m_axi_arid    = '0;
  assign m_axi_araddr  = cur_addr;
  assign m_axi_arlen   = burst_beats;
  assign m_axi_arsize  = 3'($clog2(BYTES_PER_BEAT));
  assign m_axi_arburst = 2'b01;  // INCR
  assign m_axi_arvalid = (state == S_RD_ADDR);

  // ── AXI Read data channel ─────────────────────────────────
  assign m_axi_rready   = (state == S_RD_DATA) && data_out_ready;
  assign data_out_valid  = (state == S_RD_DATA) && m_axi_rvalid;
  assign data_out        = m_axi_rdata;

  // ── AXI Write address channel ──────────────────────────────
  assign m_axi_awid    = '0;
  assign m_axi_awaddr  = cur_addr;
  assign m_axi_awlen   = burst_beats;
  assign m_axi_awsize  = 3'($clog2(BYTES_PER_BEAT));
  assign m_axi_awburst = 2'b01;  // INCR
  assign m_axi_awvalid = (state == S_WR_ADDR);

  // ── AXI Write data channel ─────────────────────────────────
  assign m_axi_wdata   = data_in;
  assign m_axi_wstrb   = {AXI_STRB_W{1'b1}};
  assign m_axi_wlast   = (beat_idx == burst_beats);
  assign m_axi_wvalid  = (state == S_WR_DATA) && data_in_valid;
  assign data_in_ready  = (state == S_WR_DATA) && m_axi_wready;

  // ── AXI Write response channel ─────────────────────────────
  assign m_axi_bready  = (state == S_WR_RESP);

endmodule
