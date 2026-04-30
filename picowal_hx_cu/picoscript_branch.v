// picoscript_branch.v -- Branch unit + program counter + call stack
// Target: iCE40HX8K (Alchitry Cu)
//
// Handles: JUMP, BRANCH, CALL, RETURN, hardware FOR, SWITCH
// Program counter is (card_id, instruction_pointer) per context.
// Call stack: 8-deep per context, stored in BRAM.

`default_nettype none

module picoscript_branch (
    input  wire        clk,
    input  wire        rst_n,

    // Current context
    input  wire [2:0]  ctx_id,

    // Advance signal: only update PC when instruction completes
    input  wire        advance,

    // Instruction decode inputs
    input  wire        is_jump,
    input  wire        is_branch,
    input  wire        is_call,
    input  wire        is_return,
    input  wire        is_hw_for,
    input  wire        is_switch,
    input  wire        is_card_jump,    // cross-card (mode bit)
    input  wire [15:0] imm16,           // target address / offset
    input  wire [3:0]  rs1,             // for SWITCH: index register select
    input  wire [3:0]  rs2,             // condition code (for BRANCH)

    // ALU flags (for branch condition evaluation)
    input  wire        flag_z,          // zero
    input  wire        flag_n,          // negative
    input  wire        flag_c,          // carry
    input  wire        flag_eof,        // end of pack (from FOREACH)
    input  wire        flag_err,        // error flag

    // Register file read (for SWITCH index)
    input  wire [31:0] reg_val,         // value of register rs1

    // Hardware FOR state
    input  wire [31:0] for_counter,     // current loop counter
    input  wire [31:0] for_limit,       // loop limit
    output wire        for_done,        // loop exhausted

    // Program counter output
    output reg  [15:0] pc_card,         // current card ID
    output reg  [6:0]  pc_ip,           // instruction pointer (0-127 within card)
    output reg         pc_update,       // PC was modified (need card reload if card changed)
    output reg         need_card_load,  // card changed → must fetch from storage

    // Call stack overflow/underflow
    output reg         stack_overflow,
    output reg         stack_underflow
);

    // ─── Per-context PC state (8 contexts) ───────────────────────────
    // Stored in registers (small enough — 8 × 23 bits = 184 bits)
    reg [15:0] ctx_card [0:7];
    reg [6:0]  ctx_ip   [0:7];

    // ─── Call stack (8 contexts × 8 depth × 23 bits) ─────────────────
    // 23 bits per entry: [22:7] = card_id, [6:0] = ip
    // Total: 8 × 8 × 23 = 1472 bits → fits in 1 BRAM block
    reg [22:0] call_stack [0:63];   // [ctx_id:3][depth:3] = 6-bit address
    reg [2:0]  stack_ptr [0:7];     // per-context stack pointer

    // ─── Branch condition evaluation ─────────────────────────────────
    reg branch_taken;

    always @(*) begin
        case (rs2[3:0])
            4'h0: branch_taken = flag_z;                    // EQ (zero set)
            4'h1: branch_taken = ~flag_z;                   // NE (zero clear)
            4'h2: branch_taken = flag_n & ~flag_z;          // LT (negative, not zero)
            4'h3: branch_taken = ~flag_n & ~flag_z;         // GT (not negative, not zero)
            4'h4: branch_taken = flag_n | flag_z;           // LE (negative or zero)
            4'h5: branch_taken = ~flag_n | flag_z;          // GE (not negative or zero)
            4'h6: branch_taken = flag_z;                    // Z (explicit zero)
            4'h7: branch_taken = ~flag_z;                   // NZ (explicit not zero)
            4'h8: branch_taken = flag_eof;                  // EOF
            4'h9: branch_taken = flag_err;                  // ERR
            default: branch_taken = 1'b0;
        endcase
    end

    // ─── Hardware FOR: compare counter vs limit ──────────────────────
    assign for_done = (for_counter >= for_limit);

    // ─── Main logic ──────────────────────────────────────────────────
    wire [5:0] stack_addr = {ctx_id, stack_ptr[ctx_id]};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pc_update       <= 1'b0;
            need_card_load  <= 1'b0;
            stack_overflow  <= 1'b0;
            stack_underflow <= 1'b0;
            begin : rst_loop
                integer i;
                for (i = 0; i < 8; i = i + 1) begin
                    ctx_card[i]  <= 16'd0;
                    ctx_ip[i]    <= 7'd0;
                    stack_ptr[i] <= 3'd0;
                end
            end
        end else begin
            pc_update       <= 1'b0;
            need_card_load  <= 1'b0;
            stack_overflow  <= 1'b0;
            stack_underflow <= 1'b0;

            // Output current PC (always visible)
            pc_card <= ctx_card[ctx_id];
            pc_ip   <= ctx_ip[ctx_id];

            // Only update PC when advance is asserted
            if (advance) begin

            // ─── JUMP (unconditional) ────────────────────────────────
            if (is_jump & ~is_switch) begin
                if (is_card_jump) begin
                    // Cross-card jump: imm16 = target card ID
                    ctx_card[ctx_id] <= imm16;
                    ctx_ip[ctx_id]   <= 7'd0;
                    need_card_load   <= 1'b1;
                end else begin
                    // Local jump: imm16[6:0] = target IP within card
                    ctx_ip[ctx_id] <= imm16[6:0];
                end
                pc_update <= 1'b1;
            end

            // ─── SWITCH (indexed jump table) ─────────────────────────
            else if (is_switch) begin
                // reg_val = index register value
                // imm16 = base address of jump table in BRAM
                // Result IP comes from BRAM[base + index] (handled externally)
                // For now: local jump to table[index]
                ctx_ip[ctx_id] <= reg_val[6:0];
                pc_update      <= 1'b1;
            end

            // ─── BRANCH (conditional) ────────────────────────────────
            else if (is_branch & ~is_hw_for) begin
                if (branch_taken) begin
                    if (is_card_jump) begin
                        ctx_card[ctx_id] <= imm16;
                        ctx_ip[ctx_id]   <= 7'd0;
                        need_card_load   <= 1'b1;
                    end else begin
                        ctx_ip[ctx_id] <= imm16[6:0];
                    end
                    pc_update <= 1'b1;
                end else begin
                    // Not taken: advance IP
                    ctx_ip[ctx_id] <= ctx_ip[ctx_id] + 7'd1;
                    pc_update      <= 1'b1;
                end
            end

            // ─── Hardware FOR (branch back if counter < limit) ───────
            else if (is_hw_for) begin
                if (!for_done) begin
                    // Loop: branch back to body start
                    ctx_ip[ctx_id] <= imm16[6:0];
                    pc_update      <= 1'b1;
                end else begin
                    // Done: fall through
                    ctx_ip[ctx_id] <= ctx_ip[ctx_id] + 7'd1;
                    pc_update      <= 1'b1;
                end
            end

            // ─── CALL (push + jump) ──────────────────────────────────
            else if (is_call) begin
                if (stack_ptr[ctx_id] == 3'd7) begin
                    stack_overflow <= 1'b1;
                end else begin
                    // Push current (card, ip+1) onto stack
                    call_stack[stack_addr] <= {ctx_card[ctx_id], ctx_ip[ctx_id] + 7'd1};
                    stack_ptr[ctx_id]      <= stack_ptr[ctx_id] + 3'd1;

                    // Jump to target
                    if (is_card_jump) begin
                        ctx_card[ctx_id] <= imm16;
                        ctx_ip[ctx_id]   <= 7'd0;
                        need_card_load   <= 1'b1;
                    end else begin
                        ctx_ip[ctx_id] <= imm16[6:0];
                    end
                    pc_update <= 1'b1;
                end
            end

            // ─── RETURN (pop + resume) ───────────────────────────────
            else if (is_return) begin
                if (stack_ptr[ctx_id] == 3'd0) begin
                    stack_underflow <= 1'b1;
                end else begin
                    stack_ptr[ctx_id] <= stack_ptr[ctx_id] - 3'd1;
                    // Pop: restore card + IP from stack
                    ctx_card[ctx_id] <= call_stack[{ctx_id, stack_ptr[ctx_id] - 3'd1}][22:7];
                    ctx_ip[ctx_id]   <= call_stack[{ctx_id, stack_ptr[ctx_id] - 3'd1}][6:0];
                    if (call_stack[{ctx_id, stack_ptr[ctx_id] - 3'd1}][22:7] != ctx_card[ctx_id])
                        need_card_load <= 1'b1;
                    pc_update <= 1'b1;
                end
            end

            // ─── Default: advance IP ─────────────────────────────────
            else begin
                ctx_ip[ctx_id] <= ctx_ip[ctx_id] + 7'd1;
                pc_update      <= 1'b1;
            end

            end // if (advance)
        end
    end

endmodule
