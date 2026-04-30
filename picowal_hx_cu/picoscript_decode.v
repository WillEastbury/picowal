// picoscript_decode.v -- PicoScript 4-bit opcode decoder
// Pure combinatorial decode of all 16 opcodes + mode extensions
// Target: iCE40HX8K (Alchitry Cu)
//
// Instruction format: [31:28]=opcode [27:24]=Rd [23:20]=Rs1 [19:16]=Rs2 [15:0]=imm16

`default_nettype none

module picoscript_decode (
    input  wire [31:0] instruction,     // 32-bit instruction word
    input  wire        valid,           // instruction is valid (fetched)

    // Decoded fields
    output wire [3:0]  opcode,
    output wire [3:0]  rd,              // destination register
    output wire [3:0]  rs1,             // source register 1
    output wire [3:0]  rs2,             // source register 2 / mode / condition
    output wire [15:0] imm16,           // immediate value

    // Opcode category (one-hot for routing)
    output wire        is_noop,
    output wire        is_load,
    output wire        is_save,
    output wire        is_pipe,
    output wire        is_add,
    output wire        is_sub,
    output wire        is_mul,
    output wire        is_div,
    output wire        is_inc,
    output wire        is_jump,
    output wire        is_branch,
    output wire        is_call,
    output wire        is_return,
    output wire        is_wait,
    output wire        is_raise,
    output wire        is_dsp,

    // Mode detection (special addressing modes)
    output wire        is_http_ctrl,    // NOOP with imm16[15]=1 → Net.*
    output wire        is_field,        // LOAD with rs2=5 → FIELD extract
    output wire        is_foreach,      // LOAD with rs2=4 → hardware FOREACH
    output wire        is_template,     // PIPE with rs2=2 → template render
    output wire        is_hw_for,       // BRANCH with rs2=4 → hardware FOR
    output wire        is_switch,       // JUMP with rs2=1 → SWITCH table
    output wire        is_fork,         // RAISE with imm16[15]=1 → parallel fork
    output wire        is_join,         // WAIT with imm16[15]=1 → join barrier
    output wire        is_card_jump,    // JUMP/CALL/BRANCH with rs2[3]=1 → cross-card

    // Execution unit select (one-hot)
    output wire        sel_alu,         // ADD/SUB/INC
    output wire        sel_mul,         // MUL (shift-add, multi-cycle)
    output wire        sel_div,         // DIV (restoring, multi-cycle)
    output wire        sel_mem,         // LOAD/SAVE/FIELD/FOREACH
    output wire        sel_pipe,        // PIPE/TEMPLATE
    output wire        sel_flow,        // JUMP/BRANCH/CALL/RETURN/SWITCH/FOR
    output wire        sel_sched,       // WAIT/RAISE/FORK/JOIN/NOOP
    output wire        sel_dsp,         // DSP sub-operations
    output wire        sel_http,        // Net.* HTTP control

    // Branch condition decode (for BRANCH opcode)
    output wire        br_eq,           // ==
    output wire        br_ne,           // !=
    output wire        br_lt,           // <
    output wire        br_gt,           // >
    output wire        br_le,           // <=
    output wire        br_ge,           // >=
    output wire        br_z,            // zero flag
    output wire        br_nz,           // not zero
    output wire        br_eof,          // end of pack
    output wire        br_err,          // error flag

    // DSP sub-op decode (for DSP opcode)
    output wire [3:0]  dsp_subop,       // Rs2 field = DSP operation select

    // Register file control
    output wire        rf_we,           // register file write enable
    output wire [3:0]  rf_waddr,        // write address (= Rd)
    output wire [3:0]  rf_raddr1,       // read address 1 (= Rs1)
    output wire [3:0]  rf_raddr2        // read address 2 (= Rs2 or imm reg)
);

    // ─── Field extraction (purely combinatorial) ─────────────────────
    assign opcode = instruction[31:28];
    assign rd     = instruction[27:24];
    assign rs1    = instruction[23:20];
    assign rs2    = instruction[19:16];
    assign imm16  = instruction[15:0];

    // ─── Opcode decode (4-bit → 16 one-hot) ─────────────────────────
    assign is_noop   = valid & (opcode == 4'h0);
    assign is_load   = valid & (opcode == 4'h1);
    assign is_save   = valid & (opcode == 4'h2);
    assign is_pipe   = valid & (opcode == 4'h3);
    assign is_add    = valid & (opcode == 4'h4);
    assign is_sub    = valid & (opcode == 4'h5);
    assign is_mul    = valid & (opcode == 4'h6);
    assign is_div    = valid & (opcode == 4'h7);
    assign is_inc    = valid & (opcode == 4'h8);
    assign is_jump   = valid & (opcode == 4'h9);
    assign is_branch = valid & (opcode == 4'hA);
    assign is_call   = valid & (opcode == 4'hB);
    assign is_return = valid & (opcode == 4'hC);
    assign is_wait   = valid & (opcode == 4'hD);
    assign is_raise  = valid & (opcode == 4'hE);
    assign is_dsp    = valid & (opcode == 4'hF);

    // ─── Mode detection (special addressing modes) ───────────────────
    assign is_http_ctrl = is_noop & imm16[15];
    assign is_field     = is_load & (rs2 == 4'h5);
    assign is_foreach   = is_load & (rs2 == 4'h4);
    assign is_template  = is_pipe & (rs2 == 4'h2);
    assign is_hw_for    = is_branch & (rs2 == 4'h4);
    assign is_switch    = is_jump & (rs2 == 4'h1);
    assign is_fork      = is_raise & imm16[15];
    assign is_join      = is_wait & imm16[15];
    assign is_card_jump = (is_jump | is_call | is_branch) & rs2[3];

    // ─── Execution unit routing ──────────────────────────────────────
    assign sel_alu   = is_add | is_sub | is_inc;
    assign sel_mul   = is_mul;
    assign sel_div   = is_div;
    assign sel_mem   = (is_load & ~is_field & ~is_foreach) | is_save;
    assign sel_pipe  = is_pipe & ~is_template;
    assign sel_flow  = is_jump | is_branch | is_call | is_return |
                       is_switch | is_hw_for | is_foreach | is_field | is_template;
    assign sel_sched = (is_noop & ~is_http_ctrl) | is_wait | is_raise |
                       is_fork | is_join;
    assign sel_dsp   = is_dsp;
    assign sel_http  = is_http_ctrl;

    // ─── Branch condition decode (Rs1 field when opcode=BRANCH) ──────
    // Condition is encoded in Rs2[3:0] for normal branch,
    // but for hw_for mode, Rs2=4 so we skip condition decode
    wire [3:0] condition = is_branch ? rs2 : 4'h0;

    assign br_eq  = (condition == 4'h0);
    assign br_ne  = (condition == 4'h1);
    assign br_lt  = (condition == 4'h2);
    assign br_gt  = (condition == 4'h3);
    assign br_le  = (condition == 4'h4);
    assign br_ge  = (condition == 4'h5);
    assign br_z   = (condition == 4'h6);
    assign br_nz  = (condition == 4'h7);
    assign br_eof = (condition == 4'h8);
    assign br_err = (condition == 4'h9);

    // ─── DSP sub-operation (Rs2 field when opcode=DSP) ───────────────
    assign dsp_subop = is_dsp ? rs2 : 4'h0;

    // ─── Register file control ───────────────────────────────────────
    // Write-back: ALU ops, LOAD, FIELD, MUL, DIV, INC write to Rd
    assign rf_we    = sel_alu | sel_mul | sel_div | is_load | is_field;
    assign rf_waddr = rd;
    assign rf_raddr1 = rs1;
    assign rf_raddr2 = rs2;

endmodule
