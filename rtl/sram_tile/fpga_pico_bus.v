// fpga_pico_bus.v — 8-bit parallel bus controller (FPGA side)
//
// Deterministic byte-at-a-time interface between FPGA and RP2354B picos.
// Each pico gets its own bus instance. No arbitration, no contention.
//
// Protocol:
//   RX (FPGA → Pico): FPGA drives DATA, asserts RDY. Pico pulses ACK.
//   TX (Pico → FPGA): Pico drives DATA, asserts RDY. FPGA pulses ACK.
//   DIR pin owned by FPGA — selects bus direction.
//   One dead cycle on DIR change (turn-around).
//
// Frame format (both directions):
//   SOF=1 on first byte, EOF=1 on last byte.
//   SOCK[1:0] = socket/stream ID (0-3), valid with SOF.
//
// Pins per pico: DATA[7:0] + RDY + ACK + DIR + SOF + EOF + SOCK[1:0] = 15
//
// Timeout: if ACK not received within TIMEOUT_CYCLES, bus resets with error flag.
//
`default_nettype none

module fpga_pico_bus #(
    parameter FIFO_DEPTH    = 512,   // bytes per direction
    parameter TIMEOUT_CYCLES = 1024  // ACK timeout
)(
    input  wire        clk,
    input  wire        rst_n,

    // --- Physical bus pins (directly to pico GPIO) ---
    inout  wire [7:0]  bus_data,     // bidirectional data
    output reg         bus_rdy,      // sender asserts when byte valid
    input  wire        bus_ack,      // receiver pulses when byte taken
    output reg         bus_dir,      // 0=FPGA→Pico (RX), 1=Pico→FPGA (TX)
    output reg         bus_sof,      // start of frame
    output reg         bus_eof,      // end of frame
    output reg  [1:0]  bus_sock,     // socket ID

    // --- Internal FPGA interface (TX to pico) ---
    input  wire [7:0]  tx_data,      // byte to send to pico
    input  wire        tx_valid,     // byte available
    output wire        tx_ready,     // bus can accept byte
    input  wire        tx_sof,       // start of frame marker
    input  wire        tx_eof,       // end of frame marker
    input  wire [1:0]  tx_sock,      // socket for this frame

    // --- Internal FPGA interface (RX from pico) ---
    output reg  [7:0]  rx_data,      // byte received from pico
    output reg         rx_valid,     // byte available
    input  wire        rx_ready,     // downstream can accept
    output reg         rx_sof,       // start of frame
    output reg         rx_eof,       // end of frame
    output reg  [1:0]  rx_sock,      // socket ID from frame

    // --- Status ---
    output reg         timeout_err,  // ACK timeout occurred
    output reg         bus_active    // transfer in progress
);

    // ─────────────────────────────────────────────────────────────────
    // Bus direction control + tristate
    // ─────────────────────────────────────────────────────────────────
    reg [7:0] data_out;
    reg       data_oe;  // 1 = FPGA drives bus

    assign bus_data = data_oe ? data_out : 8'bz;

    // ─────────────────────────────────────────────────────────────────
    // TX FIFO (FPGA → Pico direction)
    // ─────────────────────────────────────────────────────────────────
    reg [7:0]  tx_fifo [0:FIFO_DEPTH-1];
    reg [1:0]  tx_fifo_sock [0:FIFO_DEPTH-1];
    reg        tx_fifo_sof  [0:FIFO_DEPTH-1];
    reg        tx_fifo_eof  [0:FIFO_DEPTH-1];
    reg [9:0]  tx_wptr, tx_rptr;
    wire [9:0] tx_count = tx_wptr - tx_rptr;
    wire       tx_empty = (tx_wptr == tx_rptr);
    wire       tx_full  = (tx_count == FIFO_DEPTH[9:0]);

    assign tx_ready = !tx_full;

    // TX FIFO write
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_wptr <= 0;
        end else if (tx_valid && !tx_full) begin
            tx_fifo[tx_wptr[8:0]]      <= tx_data;
            tx_fifo_sock[tx_wptr[8:0]] <= tx_sock;
            tx_fifo_sof[tx_wptr[8:0]]  <= tx_sof;
            tx_fifo_eof[tx_wptr[8:0]]  <= tx_eof;
            tx_wptr <= tx_wptr + 1;
        end
    end

    // ─────────────────────────────────────────────────────────────────
    // ACK synchronizer (pico is async to FPGA clock)
    // ─────────────────────────────────────────────────────────────────
    reg ack_r, ack_rr, ack_rrr;
    wire ack_rise = ack_rr & ~ack_rrr;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ack_r   <= 0;
            ack_rr  <= 0;
            ack_rrr <= 0;
        end else begin
            ack_r   <= bus_ack;
            ack_rr  <= ack_r;
            ack_rrr <= ack_rr;
        end
    end

    // ─────────────────────────────────────────────────────────────────
    // Bus state machine
    // ─────────────────────────────────────────────────────────────────
    localparam [2:0] S_IDLE      = 3'd0,
                     S_TX_SETUP  = 3'd1,  // drive data, assert RDY
                     S_TX_WAIT   = 3'd2,  // wait for ACK
                     S_TX_DONE   = 3'd3,  // deassert RDY, advance FIFO
                     S_RX_WAIT   = 3'd4,  // wait for pico RDY (bus_ack reused)
                     S_RX_LATCH  = 3'd5,  // latch data, pulse ACK
                     S_TURNAROUND = 3'd6; // one dead cycle on DIR change

    reg [2:0]  state;
    reg [10:0] timeout_ctr;
    reg        prev_dir;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            bus_rdy     <= 0;
            bus_dir     <= 0;
            bus_sof     <= 0;
            bus_eof     <= 0;
            bus_sock    <= 0;
            data_out    <= 0;
            data_oe     <= 0;
            tx_rptr     <= 0;
            rx_data     <= 0;
            rx_valid    <= 0;
            rx_sof      <= 0;
            rx_eof      <= 0;
            rx_sock     <= 0;
            timeout_err <= 0;
            timeout_ctr <= 0;
            bus_active  <= 0;
            prev_dir    <= 0;
        end else begin
            // Default: clear single-cycle strobes
            rx_valid <= 0;

            case (state)
                S_IDLE: begin
                    bus_rdy    <= 0;
                    bus_active <= 0;
                    timeout_ctr <= 0;

                    if (!tx_empty) begin
                        // Have data to send to pico
                        if (prev_dir != 0) begin
                            bus_dir  <= 0;
                            prev_dir <= 0;
                            data_oe  <= 1;
                            state    <= S_TURNAROUND;
                        end else begin
                            data_oe <= 1;
                            state   <= S_TX_SETUP;
                        end
                    end else begin
                        // Check if pico wants to send (DIR=1, pico drives)
                        // In RX mode, we monitor bus_ack as pico's "RDY"
                        // (pico asserts its RDY on our ACK pin when DIR=1)
                        if (prev_dir != 1) begin
                            bus_dir  <= 1;
                            prev_dir <= 1;
                            data_oe  <= 0;
                            state    <= S_TURNAROUND;
                        end else begin
                            data_oe <= 0;
                            state   <= S_RX_WAIT;
                        end
                    end
                end

                S_TURNAROUND: begin
                    // One dead cycle for bus direction change
                    state <= (bus_dir == 0) ? S_TX_SETUP : S_RX_WAIT;
                end

                S_TX_SETUP: begin
                    bus_active <= 1;
                    data_out   <= tx_fifo[tx_rptr[8:0]];
                    bus_sock   <= tx_fifo_sock[tx_rptr[8:0]];
                    bus_sof    <= tx_fifo_sof[tx_rptr[8:0]];
                    bus_eof    <= tx_fifo_eof[tx_rptr[8:0]];
                    bus_rdy    <= 1;
                    timeout_ctr <= 0;
                    state      <= S_TX_WAIT;
                end

                S_TX_WAIT: begin
                    timeout_ctr <= timeout_ctr + 1;
                    if (ack_rise) begin
                        state <= S_TX_DONE;
                    end else if (timeout_ctr >= TIMEOUT_CYCLES) begin
                        timeout_err <= 1;
                        bus_rdy     <= 0;
                        state       <= S_IDLE;
                    end
                end

                S_TX_DONE: begin
                    bus_rdy <= 0;
                    bus_sof <= 0;
                    bus_eof <= 0;
                    tx_rptr <= tx_rptr + 1;
                    state   <= S_IDLE;
                end

                S_RX_WAIT: begin
                    // Pico drives DATA and asserts ACK pin (used as RDY in RX mode)
                    timeout_ctr <= timeout_ctr + 1;
                    if (ack_rise) begin
                        state <= S_RX_LATCH;
                    end else if (timeout_ctr >= TIMEOUT_CYCLES) begin
                        // No data from pico — return to idle, try TX
                        state <= S_IDLE;
                    end
                end

                S_RX_LATCH: begin
                    rx_data  <= bus_data;
                    rx_valid <= 1;
                    rx_sof   <= bus_sof;
                    rx_eof   <= bus_eof;
                    rx_sock  <= bus_sock;
                    // Pulse our RDY as ACK back to pico
                    bus_rdy  <= 1;
                    state    <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
