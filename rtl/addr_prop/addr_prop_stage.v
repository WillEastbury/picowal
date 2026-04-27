// addr_prop_stage.v — One stage of the address propagation engine
//
// This is the fundamental unit: address in → lookup → address out.
// The BRAM/SRAM contents define the transition function.
// The stage itself is just a registered lookup — no computation.
//
// COMPUTATION HAPPENS HERE: memory[addr_in] → addr_out
// The "program" is the memory contents. The "execution" is the lookup.
//
// Latency: exactly 2 clock cycles (BRAM sync read → register output)
// Throughput: 1 address per clock (fully pipelined)
//
// For BRAM version: uses inferred dual-port RAM (synthesis → BRAM blocks)
// For external SRAM: replace bram[] with pin interface (addr_prop_sram_stage.v)

module addr_prop_stage #(
    parameter ADDR_W    = 16,          // address width (= state width)
    parameter STAGE_ID  = 0,           // for debug visibility
    parameter INIT_FILE = ""           // optional hex file to preload BRAM
)(
    input  wire                clk,
    input  wire                rst_n,

    // --- Pipeline interface ---
    input  wire                valid_in,
    input  wire [ADDR_W-1:0]  addr_in,

    output reg                 valid_out,
    output reg  [ADDR_W-1:0]  addr_out,

    // --- Table load interface (active during LOAD mode only) ---
    input  wire                load_en,
    input  wire [ADDR_W-1:0]  load_addr,
    input  wire [ADDR_W-1:0]  load_data,
    input  wire                load_we,

    // --- Debug ---
    output wire [ADDR_W-1:0]  dbg_last_addr  // last address looked up
);

    // =====================================================================
    // BRAM: the transition function
    //
    // This IS the computation. Each entry: memory[state] = next_state.
    // Loading this table is "programming" this stage.
    // Reading it is "executing" the transition.
    // =====================================================================

    reg [ADDR_W-1:0] mem [0:(1 << ADDR_W)-1];

    // Optional initialisation from hex file
    initial begin
        if (INIT_FILE != "") begin
            $readmemh(INIT_FILE, mem);
        end
    end

    // Load port: host writes transition table before run
    always @(posedge clk) begin
        if (load_en && load_we) begin
            mem[load_addr] <= load_data;
        end
    end

    // =====================================================================
    // Pipeline: 2 clocks per lookup
    //
    // Clock N:   addr_in presented to BRAM (combinational address)
    // Clock N+1: BRAM output registered (synchronous read = 1 clock)
    //            valid_d1 tracks valid_in through same delay
    // Clock N+2: addr_out = BRAM result, valid_out = valid_d1
    //
    // The BRAM read IS the computation. Registers are just retiming.
    // =====================================================================

    // BRAM synchronous read: address presented combinationally,
    // data available on next clock edge
    reg [ADDR_W-1:0] bram_rdata;
    always @(posedge clk) begin
        bram_rdata <= mem[addr_in];
    end

    // Valid tracks through same 2-stage pipeline as data
    reg valid_d1;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            valid_d1 <= 1'b0;
        else
            valid_d1 <= valid_in;
    end

    // Output register: captures BRAM result
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            addr_out   <= {ADDR_W{1'b0}};
            valid_out  <= 1'b0;
        end else begin
            addr_out   <= bram_rdata;
            valid_out  <= valid_d1;
        end
    end

    // Debug: expose current BRAM read address
    assign dbg_last_addr = addr_in;

endmodule
