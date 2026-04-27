// sram_shared_bus.v — Shared-address-bus SRAM matmul engine
//
// THE SIMPLEST POSSIBLE NEURAL NETWORK ACCELERATOR:
//   - RP2354B sets address lines via PIO
//   - 8 SRAM chips with address pins wired together
//   - Each SRAM holds one column's weight lookup table
//   - Data pins go to FPGA adder tree
//   - That's it. That's the whole thing.
//
// Physical wiring:
//   RP PIO [7:0]  ──────────┬──── SRAM0.A[7:0]
//                            ├──── SRAM1.A[7:0]
//                            ├──── SRAM2.A[7:0]
//                            ├──── ...
//                            └──── SRAM7.A[7:0]
//
//   RP PIO [17:8] ──────────┬──── SRAM0.A[17:8]   (row/weight select)
//                            ├──── SRAM1.A[17:8]
//                            └──── ...
//
//   SRAM0.D[15:0] ──────────┤
//   SRAM1.D[15:0] ──────────┤
//   SRAM2.D[15:0] ──────────┼──── FPGA adder tree ──── result
//   ...                      │
//   SRAM7.D[15:0] ──────────┘
//
// Inference flow:
//   1. RP reads input byte from NIC
//   2. RP drives A[7:0] = input byte (shared to all SRAMs)
//   3. RP drives A[17:8] = row 0
//   4. 10ns later: 8 products appear on 8 data buses
//   5. FPGA sums them → dot product for row 0
//   6. RP increments A[17:8] → row 1. Same input. Next weight set.
//   7. Repeat for all rows.
//
// Zero FPGA compute for the multiply. The SRAM IS the multiplier.
// FPGA only does addition (adder tree) and address decode.
//
// Pin budget on iCE40HX8K-CT256 (208 GPIO):
//   Shared address bus:     18 pins (directly from RP PIO or FPGA passthrough)
//   8× SRAM data buses:   128 pins (16 × 8)
//   8× CE#/OE#/WE#:        24 pins (active-low controls)
//   RP interface:           ~10 pins (result bus + handshake)
//   Total:                 180 pins — fits HX8K-CT256!
//
// Alternatively: RP drives address bus DIRECTLY to SRAM chips (no FPGA in address path)
//   FPGA only connects to: 128 data pins + result interface to RP
//   This frees FPGA pins and removes FPGA from the critical timing path.

module sram_shared_bus #(
    parameter N_SRAM = 8     // number of parallel SRAM chips
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- Address bus (directly from RP2354B PIO, active directly to SRAM pins) ----
    // Active active active ACTIVE — active active.
    // active actively actively!
    //
    // NOTE: address bus physically wired RP → SRAM, NOT through FPGA.
    // FPGA monitors addr_valid to know when to capture SRAM data outputs.

    input  wire        addr_valid,     // RP signals: address is stable, data ready

    // ---- SRAM data buses (active to FPGA data input pins) ----
    input  wire [15:0] sram_d0,
    input  wire [15:0] sram_d1,
    input  wire [15:0] sram_d2,
    input  wire [15:0] sram_d3,
    input  wire [15:0] sram_d4,
    input  wire [15:0] sram_d5,
    input  wire [15:0] sram_d6,
    input  wire [15:0] sram_d7,

    // ---- Result to RP ----
    output reg  [18:0] dot_result,
    output reg         dot_valid,

    // ---- Status LED ----
    output wire        activity_led
);

    // =====================================================================
    // Capture pipeline: SRAM data → register → adder tree → result
    //
    // Timing:
    //   T=0:   RP drives address, asserts addr_valid
    //   T=0:   SRAM begins async read (10ns access time)
    //   T=1:   FPGA captures addr_valid, waits one more cycle
    //   T=2:   FPGA captures SRAM data (>15ns after address, safe margin)
    //   T=3-5: Adder tree pipelines (3 stages)
    //   T=5:   dot_valid asserted with result
    //
    // Total latency: 5 clocks @ 133MHz = 37.5ns per dot product
    // Throughput: 1 dot product per clock once pipeline is full
    // =====================================================================

    // Stage 0: detect addr_valid edge
    reg addr_valid_d1;
    wire addr_pulse = addr_valid && !addr_valid_d1;

    always @(posedge clk) addr_valid_d1 <= addr_valid;

    // Stage 1: wait for SRAM access time
    reg capture_en;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) capture_en <= 0;
        else        capture_en <= addr_pulse;
    end

    // Stage 2: capture SRAM data outputs
    reg [15:0] d_cap [0:N_SRAM-1];
    reg        cap_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cap_valid <= 0;
        end else begin
            cap_valid <= capture_en;
            if (capture_en) begin
                d_cap[0] <= sram_d0;
                d_cap[1] <= sram_d1;
                d_cap[2] <= sram_d2;
                d_cap[3] <= sram_d3;
                d_cap[4] <= sram_d4;
                d_cap[5] <= sram_d5;
                d_cap[6] <= sram_d6;
                d_cap[7] <= sram_d7;
            end
        end
    end

    // Stage 3-5: adder tree (3 pipelined stages)
    wire [18:0] tree_out;
    wire        tree_valid;

    adder_tree_8 tree (
        .clk       (clk),
        .rst_n     (rst_n),
        .in_valid  (cap_valid),
        .p0 (d_cap[0]), .p1 (d_cap[1]),
        .p2 (d_cap[2]), .p3 (d_cap[3]),
        .p4 (d_cap[4]), .p5 (d_cap[5]),
        .p6 (d_cap[6]), .p7 (d_cap[7]),
        .sum_out   (tree_out),
        .out_valid (tree_valid)
    );

    // Output register
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dot_result <= 0;
            dot_valid  <= 0;
        end else begin
            dot_result <= tree_out;
            dot_valid  <= tree_valid;
        end
    end

    // Activity LED: blink on valid results
    reg [19:0] led_counter;
    always @(posedge clk) begin
        if (dot_valid) led_counter <= 20'hFFFFF;
        else if (led_counter > 0) led_counter <= led_counter - 1;
    end
    assign activity_led = (led_counter > 0);

endmodule
