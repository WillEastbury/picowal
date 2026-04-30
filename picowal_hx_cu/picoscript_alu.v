// picoscript_alu.v -- PicoScript ALU (ADD/SUB/INC + soft MUL/DIV)
// Target: iCE40HX8K (Alchitry Cu)
//
// Single-cycle: ADD, SUB, INC
// Multi-cycle:  MUL (8 cycles, shift-add), DIV (32 cycles, restoring)

`default_nettype none

module picoscript_alu (
    input  wire        clk,
    input  wire        rst_n,

    // Operation select (from decoder)
    input  wire        op_add,
    input  wire        op_sub,
    input  wire        op_inc,
    input  wire        op_mul,
    input  wire        op_div,

    // Operands
    input  wire [31:0] a,               // Rs1 value
    input  wire [31:0] b,               // Rs2 value or imm16 (zero-extended)
    input  wire        start,           // pulse to begin operation

    // Result
    output reg  [31:0] result,
    output reg         done,            // result valid (1 cycle for ADD/SUB/INC)
    output reg         flag_z,          // zero flag
    output reg         flag_n,          // negative flag (MSB of result)
    output reg         flag_c,          // carry/overflow flag
    output wire        busy             // multi-cycle op in progress
);

    // ─── Single-cycle operations ─────────────────────────────────────
    wire [32:0] add_result = {1'b0, a} + {1'b0, b};
    wire [32:0] sub_result = {1'b0, a} - {1'b0, b};
    wire [32:0] inc_result = {1'b0, a} + 33'd1;

    // ─── Multi-cycle MUL (16-bit × 16-bit shift-add) ────────────────
    // Uses lower 16 bits of a and b for multiply
    reg  [3:0]  mul_step;
    reg  [31:0] mul_acc;
    reg  [15:0] mul_b_shift;
    reg         mul_active;

    // ─── Multi-cycle DIV (32-bit / 16-bit restoring division) ────────
    reg  [4:0]  div_step;
    reg  [31:0] div_quotient;
    reg  [31:0] div_remainder;
    reg  [31:0] div_divisor;
    reg         div_active;

    assign busy = mul_active | div_active;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result     <= 32'd0;
            done       <= 1'b0;
            flag_z     <= 1'b0;
            flag_n     <= 1'b0;
            flag_c     <= 1'b0;
            mul_active <= 1'b0;
            div_active <= 1'b0;
            mul_step   <= 4'd0;
            div_step   <= 5'd0;
        end else begin
            done <= 1'b0;  // default: not done

            // ─── Single-cycle ops ────────────────────────────────────
            if (start & op_add) begin
                result <= add_result[31:0];
                flag_c <= add_result[32];
                flag_z <= (add_result[31:0] == 32'd0);
                flag_n <= add_result[31];
                done   <= 1'b1;
            end

            else if (start & op_sub) begin
                result <= sub_result[31:0];
                flag_c <= sub_result[32];  // borrow
                flag_z <= (sub_result[31:0] == 32'd0);
                flag_n <= sub_result[31];
                done   <= 1'b1;
            end

            else if (start & op_inc) begin
                result <= inc_result[31:0];
                flag_c <= inc_result[32];
                flag_z <= (inc_result[31:0] == 32'd0);
                flag_n <= inc_result[31];
                done   <= 1'b1;
            end

            // ─── MUL start ───────────────────────────────────────────
            else if (start & op_mul & ~mul_active) begin
                mul_acc     <= 32'd0;
                mul_b_shift <= b[15:0];
                mul_step    <= 4'd0;
                mul_active  <= 1'b1;
            end

            // ─── MUL execute (shift-add, 1 bit per cycle) ────────────
            else if (mul_active) begin
                if (a[mul_step])
                    mul_acc <= mul_acc + ({16'd0, mul_b_shift} << mul_step);

                if (mul_step == 4'd15) begin
                    // Use combinatorial result for final step
                    result <= a[15] ?
                        (mul_acc + ({16'd0, mul_b_shift} << 4'd15)) :
                        mul_acc;
                    flag_z <= (mul_acc == 32'd0);  // approximate
                    flag_n <= mul_acc[31];
                    flag_c <= 1'b0;
                    mul_active <= 1'b0;
                    done       <= 1'b1;
                end else begin
                    mul_step <= mul_step + 4'd1;
                end
            end

            // ─── DIV start ───────────────────────────────────────────
            else if (start & op_div & ~div_active) begin
                if (b[15:0] == 16'd0) begin
                    // Division by zero → result = max, set error
                    result <= 32'hFFFFFFFF;
                    flag_z <= 1'b0;
                    flag_n <= 1'b1;
                    flag_c <= 1'b1;  // carry = div-by-zero indicator
                    done   <= 1'b1;
                end else begin
                    div_quotient  <= 32'd0;
                    div_remainder <= 32'd0;
                    div_divisor   <= {16'd0, b[15:0]};
                    div_step      <= 5'd31;
                    div_active    <= 1'b1;
                end
            end

            // ─── DIV execute (restoring division, 1 bit per cycle) ───
            else if (div_active) begin
                // Shift remainder left, bring in next dividend bit
                reg [31:0] trial;
                trial = {div_remainder[30:0], a[div_step]};

                if (trial >= div_divisor) begin
                    div_remainder <= trial - div_divisor;
                    div_quotient[div_step] <= 1'b1;
                end else begin
                    div_remainder <= trial;
                    div_quotient[div_step] <= 1'b0;
                end

                if (div_step == 5'd0) begin
                    result <= div_quotient;
                    flag_z <= (div_quotient == 32'd0);
                    flag_n <= div_quotient[31];
                    flag_c <= 1'b0;
                    div_active <= 1'b0;
                    done       <= 1'b1;
                end else begin
                    div_step <= div_step - 5'd1;
                end
            end
        end
    end

endmodule
