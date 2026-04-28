// lvds_tile_link.v — LVDS inter-tile link for daisy-chaining
//
// Simple framed serial protocol over LVDS pairs:
//   1× LVDS clock pair (forwarded clock)
//   1× LVDS data pair  (serial data)
//
// Frame: [SYNC(8)] [VALID(1)] [DATA(16)] [CRC(8)] = 33 bits
// At 30MHz bit clock = ~909kHz frame rate per link.
// For higher throughput, use wider parallel LVDS (4 pairs = 4× faster).
//
// iCE40HX supports LVDS on Bank 3 I/O (needs specific pin pairs).

module lvds_tile_link #(
    parameter DATA_W = 16
)(
    input  wire               clk,
    input  wire               rst_n,

    // --- TX side (to next tile) ---
    input  wire               tx_valid,
    input  wire [DATA_W-1:0] tx_data,
    output reg                tx_ready,
    output wire               lvds_tx_clk,      // forwarded clock
    output reg                lvds_tx_data,      // serial data

    // --- RX side (from previous tile) ---
    input  wire               lvds_rx_clk,       // recovered clock
    input  wire               lvds_rx_data,      // serial data
    output reg                rx_valid,
    output reg  [DATA_W-1:0] rx_data
);

    localparam SYNC_PATTERN = 8'hA5;
    localparam FRAME_BITS   = 8 + 1 + DATA_W + 8;  // 33 bits

    // =====================================================================
    // TX: serialize frame
    // =====================================================================

    reg [FRAME_BITS-1:0] tx_shift;
    reg [5:0]            tx_bit_cnt;
    reg                  tx_active;

    assign lvds_tx_clk = clk;  // forward the clock

    // Simple XOR checksum
    function [7:0] calc_crc;
        input [DATA_W:0] payload;  // valid + data
        integer i;
        begin
            calc_crc = 8'h00;
            for (i = 0; i < DATA_W + 1; i = i + 1)
                calc_crc = calc_crc ^ {7'b0, payload[i]};
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_shift   <= {FRAME_BITS{1'b1}};
            tx_bit_cnt <= 6'd0;
            tx_active  <= 1'b0;
            tx_ready   <= 1'b1;
            lvds_tx_data <= 1'b1;
        end else begin
            if (tx_active) begin
                lvds_tx_data <= tx_shift[FRAME_BITS-1];
                tx_shift     <= {tx_shift[FRAME_BITS-2:0], 1'b1};
                tx_bit_cnt   <= tx_bit_cnt + 1;
                if (tx_bit_cnt == FRAME_BITS - 1) begin
                    tx_active <= 1'b0;
                    tx_ready  <= 1'b1;
                end
            end else if (tx_valid && tx_ready) begin
                tx_shift <= {SYNC_PATTERN, 1'b1, tx_data,
                             calc_crc({1'b1, tx_data})};
                tx_bit_cnt <= 6'd0;
                tx_active  <= 1'b1;
                tx_ready   <= 1'b0;
            end else begin
                lvds_tx_data <= 1'b1;  // idle high
            end
        end
    end

    // =====================================================================
    // RX: deserialize frame (using forwarded clock)
    // =====================================================================

    reg [FRAME_BITS-1:0] rx_shift;
    reg [5:0]            rx_bit_cnt;
    reg                  rx_synced;

    always @(posedge lvds_rx_clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_shift   <= {FRAME_BITS{1'b0}};
            rx_bit_cnt <= 6'd0;
            rx_synced  <= 1'b0;
            rx_valid   <= 1'b0;
            rx_data    <= {DATA_W{1'b0}};
        end else begin
            rx_valid <= 1'b0;
            rx_shift <= {rx_shift[FRAME_BITS-2:0], lvds_rx_data};

            if (!rx_synced) begin
                // Hunt for sync pattern
                if (rx_shift[FRAME_BITS-1:FRAME_BITS-8] == SYNC_PATTERN) begin
                    rx_synced  <= 1'b1;
                    rx_bit_cnt <= 6'd8;  // already have sync
                end
            end else begin
                rx_bit_cnt <= rx_bit_cnt + 1;
                if (rx_bit_cnt == FRAME_BITS - 1) begin
                    // Full frame received
                    rx_data  <= rx_shift[DATA_W+7:8];  // extract data field
                    rx_valid <= rx_shift[DATA_W+8];     // extract valid bit
                    rx_synced <= 1'b0;                  // re-sync next frame
                end
            end
        end
    end

endmodule
