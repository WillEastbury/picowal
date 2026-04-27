// weight_loader.v — Precompute weight lookup tables into switched_matmul BRAM
//
// Given a weight matrix W[M×K] as signed INT8, computes:
//   table[row][col][x] = W[row][col] * (signed INT8)x
// for all x in 0..255, and writes them into the BRAM blocks.
//
// Load time: M × K × 256 cycles (once per model, amortised over millions of inferences)
// For 8×8: 8 × 8 × 256 = 16,384 cycles = 123μs @ 133MHz

module weight_loader #(
    parameter N_ROWS  = 4,
    parameter DOT_LEN = 8
)(
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,          // pulse to begin loading

    // Weight input: feed row-major, one weight per cycle
    input  wire [7:0]  weight_in,      // signed INT8 weight value
    input  wire        weight_valid,

    // Connection to switched_matmul weight-load port
    output reg  [$clog2(N_ROWS)-1:0] wl_row,
    output reg  [2:0]                 wl_pos,
    output reg  [7:0]                 wl_addr,
    output reg  [15:0]                wl_data,
    output reg                        wl_we,

    output reg         done
);

    // State machine
    localparam IDLE    = 2'd0,
               LATCH_W = 2'd1,
               FILL    = 2'd2,
               DONE    = 2'd3;

    reg [1:0] state;
    reg [7:0] cur_weight;              // current weight being expanded
    reg [7:0] x_counter;               // 0..255 sweep
    reg [$clog2(N_ROWS)-1:0] cur_row;
    reg [2:0] cur_pos;

    // Precompute: product = cur_weight * (signed)x_counter
    wire signed [7:0]  w_s = cur_weight;
    wire signed [7:0]  x_s = x_counter;
    wire signed [15:0] product = w_s * x_s;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= IDLE;
            wl_we      <= 0;
            done       <= 0;
            cur_row    <= 0;
            cur_pos    <= 0;
            x_counter  <= 0;
            cur_weight <= 0;
        end else begin
            case (state)

            IDLE: begin
                wl_we <= 0;
                done  <= 0;
                if (start) begin
                    cur_row   <= 0;
                    cur_pos   <= 0;
                    x_counter <= 0;
                    state     <= LATCH_W;
                end
            end

            LATCH_W: begin
                // Wait for next weight value from host
                wl_we <= 0;
                if (weight_valid) begin
                    cur_weight <= weight_in;
                    x_counter  <= 0;
                    state      <= FILL;
                end
            end

            FILL: begin
                // Write product for current x value
                wl_row  <= cur_row;
                wl_pos  <= cur_pos;
                wl_addr <= x_counter;
                wl_data <= product;
                wl_we   <= 1;

                if (x_counter == 8'd255) begin
                    // Done with this weight, advance to next position
                    wl_we <= 0;
                    if (cur_pos == DOT_LEN - 1) begin
                        cur_pos <= 0;
                        if (cur_row == N_ROWS - 1) begin
                            state <= DONE;
                        end else begin
                            cur_row <= cur_row + 1;
                            state   <= LATCH_W;
                        end
                    end else begin
                        cur_pos <= cur_pos + 1;
                        state   <= LATCH_W;
                    end
                end else begin
                    x_counter <= x_counter + 1;
                end
            end

            DONE: begin
                wl_we <= 0;
                done  <= 1;
                state <= IDLE;
            end

            endcase
        end
    end

endmodule
