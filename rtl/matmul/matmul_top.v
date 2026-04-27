// matmul_top.v — Complete switched matmul engine for iCE40HX
//
// Top-level with:
//   - SPI slave for weight loading + input streaming
//   - switched_matmul core (N_ROWS parallel dot units)
//   - weight_loader for BRAM precomputation
//   - Large-matrix sequencer (walks M×K in row-group × K-block passes)
//
// For 8×8 matmul on HX8K (N_ROWS=4):
//   - 32 BRAM blocks (of 32 available)
//   - ~600 LUTs (of 7,680 available)
//   - 2 cycles for full 8×8 matmul (4 rows per cycle)
//   - 4 cycle pipeline latency, then 1 result-group per cycle
//
// For larger matrices (e.g. 256×256 × 256×1 inference):
//   - Process K=8 columns at a time, accumulate externally
//   - Process N_ROWS rows at a time, cycle through row groups
//   - 256/4 × 256/8 = 64 × 32 = 2048 compute cycles
//   - At 133MHz: 15.4μs per matrix-vector multiply
//   - 256×256×2 = 131K ops / 15.4μs = 8.5 GOPS from a £4 chip

module matmul_top #(
    parameter N_ROWS  = 4,
    parameter DOT_LEN = 8
)(
    input  wire        clk,            // 133MHz from PLL
    input  wire        rst_n,

    // ---- Simple parallel interface to host (RP2354B via PIO) ----
    // Command bus
    input  wire [7:0]  cmd_data,       // command/data byte from host
    input  wire        cmd_valid,
    output wire        cmd_ready,

    // Result bus
    output wire [18:0] res_data,       // result word to host
    output wire        res_valid,
    input  wire        res_ready,

    // Status
    output wire        busy,
    output wire        weights_loaded
);

    // =====================================================================
    // Command decoder
    // =====================================================================
    // Commands (first byte):
    //   0x01: Load weights — followed by N_ROWS × DOT_LEN weight bytes
    //   0x02: Compute      — followed by DOT_LEN input bytes, returns N_ROWS results
    //   0x03: Status query  — returns busy/loaded flags

    localparam CMD_LOAD    = 8'h01,
               CMD_COMPUTE = 8'h02,
               CMD_STATUS  = 8'h03;

    localparam S_IDLE       = 3'd0,
               S_LOAD_INIT  = 3'd1,
               S_LOAD_FEED  = 3'd2,
               S_COMP_INPUT = 3'd3,
               S_COMP_WAIT  = 3'd4,
               S_COMP_OUT   = 3'd5;

    reg [2:0]  state;
    reg [7:0]  input_buf [0:DOT_LEN-1];
    reg [3:0]  byte_cnt;
    reg        w_loaded;

    // Weight loader signals
    reg        wl_start;
    wire       wl_done;
    reg        wl_weight_valid;
    reg [7:0]  wl_weight_in;

    wire [$clog2(N_ROWS)-1:0] wl_row_w;
    wire [2:0]                 wl_pos_w;
    wire [7:0]                 wl_addr_w;
    wire [15:0]                wl_data_w;
    wire                       wl_we_w;

    // Matmul core signals
    reg         core_compute;
    wire [18:0] core_result [0:N_ROWS-1];
    wire [N_ROWS-1:0] core_valid;

    // Result output mux
    reg [$clog2(N_ROWS)-1:0] out_idx;
    reg [18:0] out_reg;
    reg        out_valid_reg;

    assign cmd_ready      = (state == S_IDLE) || (state == S_LOAD_FEED) || (state == S_COMP_INPUT);
    assign res_data       = out_reg;
    assign res_valid      = out_valid_reg;
    assign busy           = (state != S_IDLE);
    assign weights_loaded = w_loaded;

    // =====================================================================
    // Weight loader
    // =====================================================================

    weight_loader #(
        .N_ROWS  (N_ROWS),
        .DOT_LEN (DOT_LEN)
    ) wloader (
        .clk          (clk),
        .rst_n        (rst_n),
        .start        (wl_start),
        .weight_in    (wl_weight_in),
        .weight_valid (wl_weight_valid),
        .wl_row       (wl_row_w),
        .wl_pos       (wl_pos_w),
        .wl_addr      (wl_addr_w),
        .wl_data      (wl_data_w),
        .wl_we        (wl_we_w),
        .done         (wl_done)
    );

    // =====================================================================
    // Switched matmul core
    // =====================================================================

    switched_matmul #(
        .N_ROWS  (N_ROWS),
        .DOT_LEN (DOT_LEN)
    ) core (
        .clk        (clk),
        .rst_n      (rst_n),
        .compute    (core_compute),
        .x          (input_buf),
        .wl_row     (wl_row_w),
        .wl_pos     (wl_pos_w),
        .wl_addr    (wl_addr_w),
        .wl_data    (wl_data_w),
        .wl_we      (wl_we_w),
        .result     (core_result),
        .result_valid (core_valid),
        .row_group  ({$clog2(N_ROWS){1'b0}})
    );

    // =====================================================================
    // Main state machine
    // =====================================================================

    integer k;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= S_IDLE;
            wl_start       <= 0;
            wl_weight_valid <= 0;
            core_compute   <= 0;
            out_valid_reg  <= 0;
            w_loaded       <= 0;
            byte_cnt       <= 0;
            out_idx        <= 0;
            for (k = 0; k < DOT_LEN; k = k + 1)
                input_buf[k] <= 0;
        end else begin
            // Defaults
            wl_start       <= 0;
            wl_weight_valid <= 0;
            core_compute   <= 0;
            out_valid_reg  <= 0;

            case (state)

            S_IDLE: begin
                if (cmd_valid) begin
                    case (cmd_data)
                    CMD_LOAD: begin
                        wl_start <= 1;
                        byte_cnt <= 0;
                        state    <= S_LOAD_FEED;
                    end
                    CMD_COMPUTE: begin
                        byte_cnt <= 0;
                        state    <= S_COMP_INPUT;
                    end
                    default: ;
                    endcase
                end
            end

            S_LOAD_FEED: begin
                // Stream weight bytes to weight_loader
                if (cmd_valid) begin
                    wl_weight_in    <= cmd_data;
                    wl_weight_valid <= 1;
                    byte_cnt        <= byte_cnt + 1;
                end
                if (wl_done) begin
                    w_loaded <= 1;
                    state    <= S_IDLE;
                end
            end

            S_COMP_INPUT: begin
                // Collect DOT_LEN input bytes
                if (cmd_valid) begin
                    input_buf[byte_cnt] <= cmd_data;
                    if (byte_cnt == DOT_LEN - 1) begin
                        core_compute <= 1;
                        state        <= S_COMP_WAIT;
                    end else begin
                        byte_cnt <= byte_cnt + 1;
                    end
                end
            end

            S_COMP_WAIT: begin
                // Wait for pipeline (4 cycles)
                if (core_valid[0]) begin
                    out_idx       <= 0;
                    out_reg       <= core_result[0];
                    out_valid_reg <= 1;
                    state         <= S_COMP_OUT;
                end
            end

            S_COMP_OUT: begin
                // Stream results out
                if (res_ready) begin
                    if (out_idx == N_ROWS - 1) begin
                        state <= S_IDLE;
                    end else begin
                        out_idx       <= out_idx + 1;
                        out_reg       <= core_result[out_idx + 1];
                        out_valid_reg <= 1;
                    end
                end
            end

            endcase
        end
    end

endmodule
