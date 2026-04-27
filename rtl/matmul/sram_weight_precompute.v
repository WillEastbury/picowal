// sram_weight_precompute.v — Precompute MAC lookup tables for systolic SRAM chain
//
// For each SRAM[k] in the chain, precompute:
//   table[row][partial][x] = requant(partial_decode(partial) + weight[row][k] * x_decode(x))
//
// Where:
//   partial_decode: 8-bit unsigned → signed offset (subtract 128)
//   x_decode:       8-bit raw → signed INT8
//   requant:        clip to 8-bit unsigned (add 128 offset)
//
// Table size per SRAM: 4 rows × 256 partial × 256 input = 262,144 entries
//   = exactly IS61WV25616BLL capacity (256K × 16)
//
// Write time: 262,144 writes per SRAM × 8 SRAMs = 2,097,152 total writes
//   At 50ns/write: ~105ms. Acceptable for model load.
//
// This module generates the write sequence for one SRAM in the chain.

module sram_weight_precompute #(
    parameter CHAIN_POS = 0    // which position in chain (0-7)
)(
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,

    // Weight for this position: weight[row][CHAIN_POS] for each of 4 rows
    input  wire [7:0]  weight [0:3],   // 4 weights (one per row)
    input  wire        weights_valid,

    // SRAM write interface
    output reg  [17:0] wr_addr,
    output reg  [15:0] wr_data,
    output reg         wr_we,
    output reg         done
);

    reg [1:0]  cur_row;
    reg [7:0]  cur_partial;
    reg [7:0]  cur_x;
    reg [1:0]  state;

    localparam IDLE = 0, FILLING = 1, DONE_ST = 2;

    // Precomputation logic (combinational)
    wire signed [8:0]  partial_signed = {1'b0, cur_partial} - 9'sd128;
    wire signed [7:0]  x_signed       = cur_x;
    wire signed [7:0]  w_signed       = weight[cur_row];
    wire signed [15:0] product        = w_signed * x_signed;
    wire signed [16:0] mac_result     = {partial_signed[8], partial_signed, 7'b0}  // scale partial up
                                       + {product[15], product};

    // Requantize: scale back down and offset to unsigned 8-bit
    wire signed [8:0]  scaled = mac_result[16:8];  // arithmetic right shift by 8
    wire signed [8:0]  biased = scaled + 9'sd128;
    wire [7:0]         requant = (biased < 0)   ? 8'd0   :
                                 (biased > 255) ? 8'd255  :
                                                  biased[7:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= IDLE;
            wr_we       <= 0;
            done        <= 0;
            cur_row     <= 0;
            cur_partial <= 0;
            cur_x       <= 0;
        end else begin
            case (state)

            IDLE: begin
                wr_we <= 0;
                done  <= 0;
                if (start && weights_valid) begin
                    cur_row     <= 0;
                    cur_partial <= 0;
                    cur_x       <= 0;
                    state       <= FILLING;
                end
            end

            FILLING: begin
                // Write one entry per clock
                wr_addr <= {cur_row, cur_partial, cur_x};
                wr_data <= {requant, product[7:0]};  // D[15:8]=partial, D[7:0]=precision
                wr_we   <= 1;

                // Advance counters
                if (cur_x == 8'hFF) begin
                    cur_x <= 0;
                    if (cur_partial == 8'hFF) begin
                        cur_partial <= 0;
                        if (cur_row == 2'd3) begin
                            state <= DONE_ST;
                        end else begin
                            cur_row <= cur_row + 1;
                        end
                    end else begin
                        cur_partial <= cur_partial + 1;
                    end
                end else begin
                    cur_x <= cur_x + 1;
                end
            end

            DONE_ST: begin
                wr_we <= 0;
                done  <= 1;
                state <= IDLE;
            end

            endcase
        end
    end

endmodule
