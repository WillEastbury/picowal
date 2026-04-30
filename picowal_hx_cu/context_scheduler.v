// context_scheduler.v -- Round-robin 8-context scheduler
// Target: iCE40HX8K (Alchitry Cu)
//
// Manages 8 virtual cores (contexts). Each has independent state:
//   - ACTIVE:  executing instructions, in the round-robin rotation
//   - WAITING: suspended (WAIT instruction), skipped until RAISE wakes it
//   - FORKED:  executing fork body, will rejoin on JOIN
//   - IDLE:    unused context slot
//
// Round-robin skips non-ACTIVE contexts. Context switch is 1 cycle.

`default_nettype none

module context_scheduler (
    input  wire        clk,
    input  wire        rst_n,

    // Execution control
    output reg  [2:0]  active_ctx,      // currently executing context ID
    output reg         ctx_valid,       // active_ctx is valid (has work)
    input  wire        ctx_done_cycle,  // current context finished 1 instruction

    // WAIT/RAISE interface
    input  wire        cmd_wait,        // current context wants to sleep
    input  wire [3:0]  wait_mask,       // which interrupt channels to wait on
    input  wire        cmd_raise,       // fire interrupt
    input  wire [3:0]  raise_channel,   // which channel to raise
    input  wire [2:0]  raise_target,    // (optional) specific context to wake

    // Fork/Join interface
    input  wire        cmd_fork,        // current context forks
    input  wire [2:0]  fork_count,      // how many contexts to spawn (1-7)
    input  wire        cmd_join,        // current forked context hits join barrier

    // HTTP connection assignment (hardware wakes context on new connection)
    input  wire        http_new_conn,   // new HTTP request arrived
    input  wire [2:0]  http_ctx_id,     // which context to assign it to

    // Context state visibility (for debug)
    output wire [1:0]  ctx_state_0,
    output wire [1:0]  ctx_state_1,
    output wire [1:0]  ctx_state_2,
    output wire [1:0]  ctx_state_3,
    output wire [1:0]  ctx_state_4,
    output wire [1:0]  ctx_state_5,
    output wire [1:0]  ctx_state_6,
    output wire [1:0]  ctx_state_7
);

    // Context states
    localparam CTX_IDLE    = 2'd0;
    localparam CTX_ACTIVE  = 2'd1;
    localparam CTX_WAITING = 2'd2;
    localparam CTX_FORKED  = 2'd3;

    // ─── Per-context state registers ─────────────────────────────────
    reg [1:0] ctx_state [0:7];
    reg [3:0] ctx_wait_mask [0:7];  // interrupt channels each ctx waits on
    reg [2:0] fork_parent;          // which context initiated the fork
    reg [7:0] fork_done_flags;      // bit per context: 1=joined

    // Debug visibility
    assign ctx_state_0 = ctx_state[0];
    assign ctx_state_1 = ctx_state[1];
    assign ctx_state_2 = ctx_state[2];
    assign ctx_state_3 = ctx_state[3];
    assign ctx_state_4 = ctx_state[4];
    assign ctx_state_5 = ctx_state[5];
    assign ctx_state_6 = ctx_state[6];
    assign ctx_state_7 = ctx_state[7];

    // ─── Round-robin scanner ─────────────────────────────────────────
    reg [2:0] rr_ptr;               // next context to check

    // Find next active context (combinatorial scan)
    wire [7:0] active_mask;
    assign active_mask = {
        (ctx_state[7] == CTX_ACTIVE) | (ctx_state[7] == CTX_FORKED),
        (ctx_state[6] == CTX_ACTIVE) | (ctx_state[6] == CTX_FORKED),
        (ctx_state[5] == CTX_ACTIVE) | (ctx_state[5] == CTX_FORKED),
        (ctx_state[4] == CTX_ACTIVE) | (ctx_state[4] == CTX_FORKED),
        (ctx_state[3] == CTX_ACTIVE) | (ctx_state[3] == CTX_FORKED),
        (ctx_state[2] == CTX_ACTIVE) | (ctx_state[2] == CTX_FORKED),
        (ctx_state[1] == CTX_ACTIVE) | (ctx_state[1] == CTX_FORKED),
        (ctx_state[0] == CTX_ACTIVE) | (ctx_state[0] == CTX_FORKED)
    };

    wire any_active = |active_mask;

    // Priority encoder: find next active after rr_ptr
    reg [2:0] next_ctx;
    reg       found;

    always @(*) begin
        found = 1'b0;
        next_ctx = rr_ptr;
        // Unrolled scan: check rr_ptr+1 through rr_ptr+8 (wrapping)
        if (!found && active_mask[(rr_ptr + 3'd1) & 3'h7]) begin next_ctx = (rr_ptr + 3'd1) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd2) & 3'h7]) begin next_ctx = (rr_ptr + 3'd2) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd3) & 3'h7]) begin next_ctx = (rr_ptr + 3'd3) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd4) & 3'h7]) begin next_ctx = (rr_ptr + 3'd4) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd5) & 3'h7]) begin next_ctx = (rr_ptr + 3'd5) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd6) & 3'h7]) begin next_ctx = (rr_ptr + 3'd6) & 3'h7; found = 1'b1; end
        if (!found && active_mask[(rr_ptr + 3'd7) & 3'h7]) begin next_ctx = (rr_ptr + 3'd7) & 3'h7; found = 1'b1; end
        if (!found && active_mask[rr_ptr])                  begin next_ctx = rr_ptr;                  found = 1'b1; end
    end

    // ─── Main state machine ──────────────────────────────────────────
    integer k;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (k = 0; k < 8; k = k + 1) begin
                ctx_state[k]     <= CTX_IDLE;
                ctx_wait_mask[k] <= 4'd0;
            end
            // Context 0 starts active (boot context)
            ctx_state[0]   <= CTX_ACTIVE;
            active_ctx     <= 3'd0;
            ctx_valid      <= 1'b1;
            rr_ptr         <= 3'd0;
            fork_parent    <= 3'd0;
            fork_done_flags <= 8'd0;
        end else begin

            // ─── Context switch on cycle complete ─────────────────────
            if (ctx_done_cycle) begin
                rr_ptr    <= active_ctx;
                active_ctx <= next_ctx;
                ctx_valid  <= found;
            end

            // ─── WAIT: suspend current context ────────────────────────
            if (cmd_wait & ~cmd_join) begin
                ctx_state[active_ctx]     <= CTX_WAITING;
                ctx_wait_mask[active_ctx] <= wait_mask;
                // Immediately switch
                rr_ptr    <= active_ctx;
                active_ctx <= next_ctx;
                ctx_valid  <= found;
            end

            // ─── RAISE: wake matching contexts ────────────────────────
            if (cmd_raise) begin
                for (k = 0; k < 8; k = k + 1) begin
                    if (ctx_state[k] == CTX_WAITING &&
                        (ctx_wait_mask[k] & raise_channel) != 4'd0) begin
                        ctx_state[k] <= CTX_ACTIVE;
                    end
                end
            end

            // ─── HTTP auto-wake: assign new connection to context ─────
            if (http_new_conn) begin
                if (ctx_state[http_ctx_id] == CTX_WAITING ||
                    ctx_state[http_ctx_id] == CTX_IDLE) begin
                    ctx_state[http_ctx_id] <= CTX_ACTIVE;
                end
            end

            // ─── FORK: spawn contexts for parallel loop ───────────────
            if (cmd_fork) begin
                fork_parent     <= active_ctx;
                fork_done_flags <= 8'd0;
                // Wake idle contexts (simplified: always fork all idle)
                for (k = 0; k < 8; k = k + 1) begin
                    if (ctx_state[k] == CTX_IDLE)
                        ctx_state[k] <= CTX_FORKED;
                end
                // Parent suspends until join
                ctx_state[active_ctx] <= CTX_WAITING;
            end

            // ─── JOIN: forked context completes ───────────────────────
            if (cmd_join) begin
                fork_done_flags[active_ctx] <= 1'b1;
                ctx_state[active_ctx]       <= CTX_IDLE;

                // Check if all forked contexts are now done
                // Simple: if no CTX_FORKED remains (other than us), wake parent
                if (ctx_state[0] != CTX_FORKED &&
                    ctx_state[1] != CTX_FORKED &&
                    ctx_state[2] != CTX_FORKED &&
                    ctx_state[3] != CTX_FORKED &&
                    ctx_state[4] != CTX_FORKED &&
                    ctx_state[5] != CTX_FORKED &&
                    ctx_state[6] != CTX_FORKED &&
                    ctx_state[7] != CTX_FORKED) begin
                    ctx_state[fork_parent] <= CTX_ACTIVE;
                end
            end
        end
    end

endmodule
