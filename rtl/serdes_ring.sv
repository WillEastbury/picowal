// serdes_ring.sv — Inter-FPGA SERDES ring interconnect
// Ring topology: FPGA0 → FPGA1 → FPGA2 → FPGA3 → FPGA0
// Used for activation/psum forwarding between inference slices
// Each link: 4 lanes × 5 Gbps = 2.5 GB/s per direction

module serdes_ring
  import hydra_infer_pkg::*;
#(
  parameter slice_id_t THIS_SLICE = 0
)(
  input  logic    clk,
  input  logic    rst_n,

  // ── SERDES PHY interface (to next slice in ring) ───────────
  output logic [SERDES_LANES-1:0] tx_data_p,
  output logic [SERDES_LANES-1:0] tx_data_n,
  input  logic [SERDES_LANES-1:0] rx_data_p,
  input  logic [SERDES_LANES-1:0] rx_data_n,

  // ── TX: local slice wants to send a packet ─────────────────
  input  logic                     tx_valid,
  output logic                     tx_ready,
  input  ring_pkt_t                tx_pkt,

  // ── RX: packet arrived for this slice ──────────────────────
  output logic                     rx_valid,
  input  logic                     rx_ready,
  output ring_pkt_t                rx_pkt,

  // ── Forward: packet passing through (not for us) ───────────
  // Automatically forwarded to TX — no action needed from user logic

  // ── Status ─────────────────────────────────────────────────
  output logic                     link_up,
  output logic [31:0]              tx_pkt_count,
  output logic [31:0]              rx_pkt_count
);

  // ── TX/RX FIFOs ────────────────────────────────────────────
  ring_pkt_t tx_fifo [8];
  ring_pkt_t rx_fifo [8];
  logic [2:0] tx_wr, tx_rd, rx_wr, rx_rd;
  logic [3:0] tx_count, rx_count;

  // ── Link state machine ─────────────────────────────────────
  typedef enum logic [1:0] {
    LINK_DOWN,
    LINK_TRAINING,
    LINK_UP
  } link_state_t;

  link_state_t lstate;
  logic [15:0] train_counter;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      lstate        <= LINK_DOWN;
      train_counter <= '0;
    end else begin
      case (lstate)
        LINK_DOWN: begin
          train_counter <= '0;
          lstate        <= LINK_TRAINING;
        end
        LINK_TRAINING: begin
          train_counter <= train_counter + 1;
          if (train_counter == 16'hFFFF)
            lstate <= LINK_UP;
        end
        LINK_UP: lstate <= LINK_UP;
        default: lstate <= LINK_DOWN;
      endcase
    end
  end

  assign link_up = (lstate == LINK_UP);

  // ── TX path ────────────────────────────────────────────────
  assign tx_ready = link_up && (tx_count < 7);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      tx_wr       <= '0;
      tx_count    <= '0;
      tx_pkt_count <= '0;
    end else begin
      logic do_wr, do_rd;
      do_wr = tx_valid && tx_ready;
      do_rd = link_up && (tx_count > 0);  // serialize out

      if (do_wr) begin
        tx_fifo[tx_wr] <= tx_pkt;
        tx_wr          <= tx_wr + 1;
        tx_pkt_count   <= tx_pkt_count + 1;
      end

      case ({do_wr, do_rd})
        2'b10:   tx_count <= tx_count + 1;
        2'b01:   tx_count <= tx_count - 1;
        default: ;
      endcase
    end
  end

  // ── RX path ────────────────────────────────────────────────
  // Incoming packets: if dst == THIS_SLICE, deliver to rx_pkt
  // Otherwise, forward to TX (ring forwarding)

  ring_pkt_t rx_raw;
  logic      rx_raw_valid;

  // Simulated deserialization (real impl uses ECP5 DCUA hard macro)
  assign rx_raw_valid = link_up;  // placeholder
  assign rx_raw       = '0;       // placeholder — real: from SERDES deserializer

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      rx_wr       <= '0;
      rx_rd       <= '0;
      rx_count    <= '0;
      rx_pkt_count <= '0;
    end else begin
      // Deliver to local
      if (rx_valid && rx_ready) begin
        rx_rd    <= rx_rd + 1;
        rx_count <= rx_count - 1;
      end

      // Receive from PHY
      if (rx_raw_valid && rx_raw.dst == THIS_SLICE && rx_count < 7) begin
        rx_fifo[rx_wr] <= rx_raw;
        rx_wr          <= rx_wr + 1;
        rx_count       <= rx_count + 1;
        rx_pkt_count   <= rx_pkt_count + 1;
      end
      // Forward packets not addressed to us (ring pass-through handled in TX)
    end
  end

  assign rx_valid = (rx_count > 0);
  assign rx_pkt   = rx_fifo[rx_rd];

  // ── SERDES PHY output (placeholder — maps to ECP5 DCUA) ───
  assign tx_data_p = '0;
  assign tx_data_n = '1;

endmodule
