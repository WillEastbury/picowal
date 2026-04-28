// sram_bus_ctrl.v — Controls one bus of 8 SRAMs with chip-select decode
//
// Shared address/data bus, active CE# selects one SRAM at a time.
// In RUN mode: sequences through 8 stages as a pipeline.
// In LOAD mode: host writes to addressed chip via load interface.
//
// Timing: 2 clocks per SRAM access at 30MHz (33ns per access)
//   Clock 1: drive address + assert CE# → SRAM starts access
//   Clock 2: capture data from SRAM → next stage input
// Full 8-stage sweep = 16 clocks = 533ns @ 30MHz

module sram_bus_ctrl #(
    parameter DATA_W    = 16,
    parameter ADDR_W    = 16,       // pipeline state width
    parameter SRAM_AW   = 16,       // physical SRAM address pins
    parameter N_CHIPS   = 8,
    parameter BUS_ID    = 0
)(
    input  wire                clk,
    input  wire                rst_n,

    // --- Pipeline interface ---
    input  wire                pipe_valid_in,
    input  wire [DATA_W-1:0]  pipe_data_in,     // input to first stage
    output reg                 pipe_valid_out,
    output reg  [DATA_W-1:0]  pipe_data_out,    // output from last stage

    // --- Mode ---
    input  wire                mode_run,          // 0=LOAD, 1=RUN

    // --- Load interface ---
    input  wire [2:0]          load_chip_sel,     // which chip (0-7)
    input  wire [SRAM_AW-1:0] load_addr,
    input  wire [DATA_W-1:0]  load_data,
    input  wire                load_we,

    // --- Physical SRAM bus pins ---
    output reg  [SRAM_AW-1:0] sram_a,
    inout  wire [DATA_W-1:0]  sram_dq,
    output reg  [N_CHIPS-1:0] sram_ce_n,         // one CE# per chip
    output wire                sram_oe_n,
    output wire                sram_we_n,

    // --- Status ---
    output wire                busy,              // pipeline sweep in progress
    output wire [3:0]          dbg_stage
);

    // =====================================================================
    // State machine
    // =====================================================================

    localparam S_IDLE    = 2'd0;
    localparam S_ADDR    = 2'd1;    // drive address, assert CE#
    localparam S_CAPTURE = 2'd2;    // capture SRAM data

    reg [1:0]          state;
    reg [2:0]          chip_idx;    // current chip (0 to N_CHIPS-1)
    reg [DATA_W-1:0]  stage_data [0:N_CHIPS-1];  // pipeline registers
    reg                sweep_active;

    // Load mode tristate
    wire load_active = !mode_run && load_we;

    assign sram_dq   = load_active ? load_data : {DATA_W{1'bz}};
    assign sram_oe_n = load_active ? 1'b1 : 1'b0;
    assign sram_we_n = load_active ? 1'b0 : 1'b1;
    assign busy      = sweep_active;
    assign dbg_stage = {1'b0, chip_idx};

    integer i;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            chip_idx     <= 3'd0;
            sweep_active <= 1'b0;
            pipe_valid_out <= 1'b0;
            pipe_data_out  <= {DATA_W{1'b0}};
            sram_a       <= {SRAM_AW{1'b0}};
            sram_ce_n    <= {N_CHIPS{1'b1}};
            for (i = 0; i < N_CHIPS; i = i + 1)
                stage_data[i] <= {DATA_W{1'b0}};
        end else if (!mode_run) begin
            // === LOAD MODE ===
            state        <= S_IDLE;
            sweep_active <= 1'b0;
            pipe_valid_out <= 1'b0;
            sram_ce_n    <= {N_CHIPS{1'b1}};

            if (load_we) begin
                sram_a    <= load_addr;
                sram_ce_n <= ~({{(N_CHIPS-1){1'b0}}, 1'b1} << load_chip_sel);
            end
        end else begin
            // === RUN MODE ===
            case (state)
                S_IDLE: begin
                    pipe_valid_out <= 1'b0;
                    if (pipe_valid_in) begin
                        stage_data[0] <= pipe_data_in;
                        chip_idx      <= 3'd0;
                        sweep_active  <= 1'b1;
                        state         <= S_ADDR;
                    end
                end

                S_ADDR: begin
                    // Drive address from current stage register
                    sram_a    <= {{(SRAM_AW-DATA_W){1'b0}}, stage_data[chip_idx]};
                    sram_ce_n <= ~({{(N_CHIPS-1){1'b0}}, 1'b1} << chip_idx);
                    state     <= S_CAPTURE;
                end

                S_CAPTURE: begin
                    // Capture SRAM output into next stage register
                    sram_ce_n <= {N_CHIPS{1'b1}};

                    if (chip_idx == N_CHIPS - 1) begin
                        // Last chip — output result
                        pipe_data_out  <= sram_dq[DATA_W-1:0];
                        pipe_valid_out <= 1'b1;
                        sweep_active   <= 1'b0;
                        state          <= S_IDLE;
                    end else begin
                        stage_data[chip_idx + 1] <= sram_dq[DATA_W-1:0];
                        chip_idx <= chip_idx + 1;
                        state    <= S_ADDR;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
