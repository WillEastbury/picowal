// mem_switch_fabric.v — Multi-tier memory crossbar with loopback
//
// THE ARCHITECTURE:
//   FPGA is a ROUTING SWITCH, not a compute element.
//   Multiple tiers of memory (SRAM, NOR flash, LPDDR4) hold jump tables.
//   FPGA selects which bank to address, captures the result, and
//   LOOPS IT BACK as the input for the next step.
//
//   The "program" is the routing sequence in the FPGA state machine.
//   The "computation" is the memory lookup.
//   The weights ARE the jump tables in memory.
//   The model IS the routing schedule.
//
// Memory tiers:
//   Tier 0 — SRAM:  10ns, hot path (embeddings, attention, frequent layers)
//   Tier 1 — NOR:   70ns, bulk weights (large FC layers, cold path)
//   Tier 2 — DRAM:  2.5ns/col (full model, deep storage, page-hit fast)
//
// Loopback:
//   Output of any memory read feeds back to the fabric input.
//   FPGA state machine advances to next step, selects next bank/address.
//   Same physical memory reused for every layer — no chip-per-layer.
//
// For a 4-layer network with 8-wide dot products:
//   Step 0-7:   Layer 0 weights in SRAM bank A, loopback partial sums
//   Step 8-15:  Layer 1 weights in SRAM bank B, loopback partial sums
//   Step 16-23: Layer 2 weights in NOR flash, loopback partial sums
//   Step 24-31: Layer 3 weights in NOR flash, loopback partial sums
//   Total: 32 steps, mixed latency based on tier hit

module mem_switch_fabric #(
    parameter N_SRAM    = 4,     // number of SRAM chips
    parameter N_NOR     = 4,     // number of NOR flash chips
    parameter HAS_DRAM  = 1,     // 1 if LPDDR4 attached
    parameter ADDR_W    = 21,    // max address width (NOR flash = 21)
    parameter DATA_W    = 16,    // data bus width
    parameter MAX_STEPS = 64     // max steps in a routing schedule
)(
    input  wire        clk,
    input  wire        rst_n,

    // ---- External input (from NIC / RP2354B) ----
    input  wire [7:0]  ext_input,
    input  wire        ext_valid,

    // ---- Loopback bus (active from memory data → fabric input) ----
    wire [DATA_W-1:0]  loopback;

    // ---- SRAM interfaces (active to chip pins) ----
    output reg  [ADDR_W-1:0]   sram_addr  [0:N_SRAM-1],
    input  wire [DATA_W-1:0]   sram_data  [0:N_SRAM-1],
    output reg  [N_SRAM-1:0]   sram_ce_n,
    output reg  [N_SRAM-1:0]   sram_oe_n,

    // ---- NOR Flash interfaces (active to chip pins) ----
    output reg  [ADDR_W-1:0]   nor_addr   [0:N_NOR-1],
    input  wire [DATA_W-1:0]   nor_data   [0:N_NOR-1],
    output reg  [N_NOR-1:0]    nor_ce_n,
    output reg  [N_NOR-1:0]    nor_oe_n,

    // ---- DRAM interface (active active through controller) ----
    output reg  [24:0]         dram_addr,
    input  wire [DATA_W-1:0]   dram_data,
    output reg                 dram_rd,
    input  wire                dram_valid,

    // ---- Result ----
    output reg  [DATA_W-1:0]   result,
    output reg                 result_valid,
    output wire                busy
);

    // =====================================================================
    // Routing schedule: stored in FPGA BRAM (or registers for small nets)
    //
    // Each step says:
    //   - Which tier to access (SRAM / NOR / DRAM)
    //   - Which chip within that tier
    //   - Upper address bits (row/weight select)
    //   - Where the lower address comes from (ext_input or loopback)
    //   - Whether to capture result or continue accumulating
    // =====================================================================

    // Schedule entry format (active in FPGA BRAM)
    // [31:30] = tier:       0=SRAM, 1=NOR, 2=DRAM
    // [29:27] = chip_sel:   which chip in tier (0-7)
    // [26:19] = addr_hi:    upper address bits (row/weight select)
    // [18]    = input_src:  0=external input, 1=loopback
    // [17]    = acc_mode:   0=new lookup, 1=accumulate (partial in addr)
    // [16]    = last_step:  1=this is the final step, capture result
    // [15:0]  = reserved

    reg [31:0] schedule [0:MAX_STEPS-1];
    reg [5:0]  step;
    reg        running;

    // Current step decode
    wire [1:0] cur_tier     = schedule[step][31:30];
    wire [2:0] cur_chip     = schedule[step][29:27];
    wire [7:0] cur_addr_hi  = schedule[step][26:19];
    wire       cur_input_src = schedule[step][18];
    wire       cur_acc_mode = schedule[step][17];
    wire       cur_last     = schedule[step][16];

    // =====================================================================
    // Loopback register: captures memory output, feeds back as next input
    // =====================================================================

    reg [DATA_W-1:0] loopback_reg;
    reg [7:0]        partial_reg;    // accumulated partial sum (from D[15:8])

    assign loopback = loopback_reg;

    // Input mux: external or loopback
    wire [7:0] cur_input = cur_input_src ? loopback_reg[7:0] : ext_input;

    // Address assembly
    // For accumulate mode: {addr_hi, partial, input}
    // For fresh lookup:    {addr_hi, 8'h80, input}   (partial=0, offset encoded)
    wire [ADDR_W-1:0] full_addr = cur_acc_mode ?
        {cur_addr_hi[ADDR_W-17:0], partial_reg, cur_input} :
        {cur_addr_hi[ADDR_W-17:0], 8'h80, cur_input};

    // =====================================================================
    // State machine
    // =====================================================================

    localparam S_IDLE    = 3'd0,
               S_SETUP   = 3'd1,
               S_WAIT    = 3'd2,
               S_CAPTURE = 3'd3,
               S_DONE    = 3'd4;

    reg [2:0]  state;
    reg [3:0]  wait_count;  // wait for memory access time

    // Tier-dependent wait cycles (at 100MHz = 10ns/cycle)
    wire [3:0] tier_wait = (cur_tier == 2'd0) ? 4'd1 :   // SRAM: 10ns = 1 cycle
                           (cur_tier == 2'd1) ? 4'd7 :   // NOR:  70ns = 7 cycles
                                                4'd0;     // DRAM: use dram_valid

    assign busy = running;

    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            running      <= 0;
            step         <= 0;
            result_valid <= 0;
            loopback_reg <= 0;
            partial_reg  <= 8'h80;
            // Deassert all chip enables
            sram_ce_n    <= {N_SRAM{1'b1}};
            sram_oe_n    <= {N_SRAM{1'b1}};
            nor_ce_n     <= {N_NOR{1'b1}};
            nor_oe_n     <= {N_NOR{1'b1}};
            dram_rd      <= 0;
        end else begin
            result_valid <= 0;

            case (state)

            S_IDLE: begin
                if (ext_valid) begin
                    running     <= 1;
                    step        <= 0;
                    partial_reg <= 8'h80;  // reset accumulator
                    state       <= S_SETUP;
                end
            end

            S_SETUP: begin
                // Deassert all
                sram_ce_n <= {N_SRAM{1'b1}};
                sram_oe_n <= {N_SRAM{1'b1}};
                nor_ce_n  <= {N_NOR{1'b1}};
                nor_oe_n  <= {N_NOR{1'b1}};
                dram_rd   <= 0;

                // Assert the selected chip
                case (cur_tier)
                2'd0: begin // SRAM
                    sram_addr[cur_chip] <= full_addr;
                    sram_ce_n[cur_chip] <= 0;
                    sram_oe_n[cur_chip] <= 0;
                end
                2'd1: begin // NOR Flash
                    nor_addr[cur_chip]  <= full_addr;
                    nor_ce_n[cur_chip]  <= 0;
                    nor_oe_n[cur_chip]  <= 0;
                end
                2'd2: begin // DRAM
                    dram_addr <= full_addr;
                    dram_rd   <= 1;
                end
                endcase

                wait_count <= 0;
                state      <= S_WAIT;
            end

            S_WAIT: begin
                if (cur_tier == 2'd2) begin
                    // DRAM: wait for controller valid signal
                    if (dram_valid)
                        state <= S_CAPTURE;
                end else begin
                    // SRAM/NOR: fixed wait
                    if (wait_count >= tier_wait)
                        state <= S_CAPTURE;
                    else
                        wait_count <= wait_count + 1;
                end
            end

            S_CAPTURE: begin
                // Grab data from the active memory
                case (cur_tier)
                2'd0: loopback_reg <= sram_data[cur_chip];
                2'd1: loopback_reg <= nor_data[cur_chip];
                2'd2: loopback_reg <= dram_data;
                endcase

                // Update partial sum from D[15:8]
                case (cur_tier)
                2'd0: partial_reg <= sram_data[cur_chip][15:8];
                2'd1: partial_reg <= nor_data[cur_chip][15:8];
                2'd2: partial_reg <= dram_data[15:8];
                endcase

                // Deassert chip selects
                sram_ce_n <= {N_SRAM{1'b1}};
                nor_ce_n  <= {N_NOR{1'b1}};
                dram_rd   <= 0;

                if (cur_last) begin
                    state <= S_DONE;
                end else begin
                    step  <= step + 1;
                    state <= S_SETUP;
                end
            end

            S_DONE: begin
                result       <= loopback_reg;
                result_valid <= 1;
                running      <= 0;
                state        <= S_IDLE;
            end

            endcase
        end
    end

    // =====================================================================
    // Schedule programming interface
    // RP2354B writes the routing schedule at startup via simple bus
    // =====================================================================

    input  wire [5:0]  sched_addr;
    input  wire [31:0] sched_data;
    input  wire        sched_we;

    always @(posedge clk) begin
        if (sched_we)
            schedule[sched_addr] <= sched_data;
    end

endmodule
