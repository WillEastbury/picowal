// addr_prop_sram_stage.v — External async SRAM version of one pipeline stage
//
// Same concept as addr_prop_stage.v but uses external SRAM pins
// instead of inferred BRAM. For use with IS61WV25616BLL (256K×16, 10ns).
//
// Pin interface to SRAM:
//   A[17:0]  = address (directly from pipeline register)
//   D[15:0]  = data (to/from next stage register)
//   CE#      = chip enable (active low)
//   OE#      = output enable (active low)
//   WE#      = write enable (active low, only during LOAD)
//
// Timing budget at 50MHz (20ns period):
//   FPGA tCO:        ~3ns
//   PCB trace:        ~1ns
//   SRAM tAA:        10ns
//   PCB return:       ~1ns
//   FPGA setup:       ~3ns
//   Margin:           ~2ns  ← comfortable
//
// At 66MHz (15ns): tight. At 80MHz (12.5ns): won't close.
// Use 50MHz for reliable operation.

module addr_prop_sram_stage #(
    parameter ADDR_W   = 16,      // pipeline address width (≤16 for this SRAM)
    parameter SRAM_AW  = 18,      // SRAM address width (IS61WV25616BLL = 18)
    parameter STAGE_ID = 0
)(
    input  wire                clk,
    input  wire                rst_n,

    // --- Pipeline interface ---
    input  wire                valid_in,
    input  wire [ADDR_W-1:0]  addr_in,
    output reg                 valid_out,
    output reg  [ADDR_W-1:0]  addr_out,

    // --- Mode ---
    input  wire                mode_run,     // 0=LOAD, 1=RUN

    // --- Load interface (directly from host) ---
    input  wire [SRAM_AW-1:0] load_addr,
    input  wire [15:0]        load_data,
    input  wire                load_we,

    // --- Physical SRAM pins ---
    output wire [SRAM_AW-1:0] sram_a,
    inout  wire [15:0]        sram_dq,
    output wire                sram_ce_n,
    output wire                sram_oe_n,
    output wire                sram_we_n,

    // --- Debug ---
    output wire [ADDR_W-1:0]  dbg_last_addr
);

    // =====================================================================
    // Address mux: LOAD mode = host address, RUN mode = pipeline address
    // =====================================================================

    reg [ADDR_W-1:0] pipe_addr_r;
    reg               pipe_valid_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pipe_addr_r  <= {ADDR_W{1'b0}};
            pipe_valid_r <= 1'b0;
        end else begin
            pipe_addr_r  <= addr_in;
            pipe_valid_r <= valid_in;
        end
    end

    // Address to SRAM: host during LOAD, pipeline during RUN
    // Upper bits tied to 0 when ADDR_W < SRAM_AW
    wire [SRAM_AW-1:0] run_addr = {{(SRAM_AW-ADDR_W){1'b0}}, pipe_addr_r};
    wire [SRAM_AW-1:0] host_addr = load_addr;

    assign sram_a = mode_run ? run_addr : host_addr;

    // =====================================================================
    // Data bus: tristate control
    //   LOAD mode: FPGA drives DQ for writes
    //   RUN mode:  SRAM drives DQ, FPGA reads
    // =====================================================================

    wire load_active = !mode_run && load_we;

    // Tristate: drive only during LOAD writes
    assign sram_dq = load_active ? load_data : 16'bz;

    // Control signals
    assign sram_ce_n = 1'b0;                         // always enabled
    assign sram_oe_n = load_active ? 1'b1 : 1'b0;   // OE off during write
    assign sram_we_n = load_active ? 1'b0 : 1'b1;   // WE on during write only

    // =====================================================================
    // Capture SRAM data output into pipeline register
    // One clock after address is presented, data is stable (at 50MHz)
    // =====================================================================

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            addr_out   <= {ADDR_W{1'b0}};
            valid_out  <= 1'b0;
        end else if (mode_run) begin
            addr_out   <= sram_dq[ADDR_W-1:0];
            valid_out  <= pipe_valid_r;
        end else begin
            addr_out   <= {ADDR_W{1'b0}};
            valid_out  <= 1'b0;
        end
    end

    assign dbg_last_addr = pipe_addr_r;

endmodule
