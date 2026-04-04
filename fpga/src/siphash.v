`timescale 1ns / 1ps
//============================================================================
// siphash.v — SipHash-2-4 Core for iCE40 HX8K
//
// SipHash-2-4: 128-bit key, variable-length message, 64-bit output.
// Each SipRound executes in one clock cycle (combinational 64-bit adds +
// rotates).  Total latency for an N-block message:
//   1 (init) + N*(1 + 2) + 1 (pad setup if aligned) + 2 (final block)
//   + 1 (fin XOR) + 4 (fin rounds) + 1 (done) cycles.
//
// Message bytes are streamed in one per cycle via msg_byte / msg_valid.
// Assert msg_last with (or without) the final msg_valid to signal end-of-
// message.  The core pads, compresses and finalises automatically.
//============================================================================

module siphash (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         start,        // pulse to begin new hash
    input  wire [127:0] key,          // 128-bit key (k0 = key[63:0])
    input  wire [7:0]   msg_byte,     // message byte input
    input  wire         msg_valid,    // pulse per byte
    input  wire         msg_last,     // asserted with final byte (or alone)
    output reg  [63:0]  hash_out,
    output reg          hash_valid,   // pulse when hash ready
    output reg          busy
);

    // ── SipHash initialisation constants ────────────────────────────────
    localparam [63:0] CONST_0 = 64'h736f6d6570736575;
    localparam [63:0] CONST_1 = 64'h646f72616e646f6d;
    localparam [63:0] CONST_2 = 64'h6c7967656e657261;
    localparam [63:0] CONST_3 = 64'h7465646279746573;

    // ── FSM states ──────────────────────────────────────────────────────
    localparam [3:0]
        S_IDLE      = 4'd0,
        S_ACCUM     = 4'd1,
        S_COMPRESS  = 4'd2,  // 2 SipRounds per message block
        S_PAD_SETUP = 4'd3,  // create extra padding block (msg aligned to 8)
        S_FIN_XOR   = 4'd4,  // v2 ^= 0xFF before finalization rounds
        S_FINALIZE  = 4'd5,  // 4 SipRounds
        S_DONE      = 4'd6;

    reg [3:0]  state;

    // ── Internal registers ──────────────────────────────────────────────
    reg [63:0] v0, v1, v2, v3;
    reg [63:0] msg_block;        // byte accumulator for current 8-byte block
    reg [63:0] current_block;    // block being compressed (saved for v0 ^= m)
    reg [2:0]  byte_cnt;         // 0-7 byte position within block
    reg [7:0]  msg_len;          // total message length mod 256
    reg [2:0]  round_cnt;        // SipRound counter
    reg        finalizing;       // current compress is the final padded block
    reg        need_pad_block;   // need an extra all-zero padding block

    // ── 64-bit left rotate (constant shift, purely combinational) ───────
    function [63:0] rotl64;
        input [63:0] x;
        input integer n;
        rotl64 = (x << n) | (x >> (64 - n));
    endfunction

    // ── SipRound — full round in combinational logic ────────────────────
    // Input : v0, v1, v2, v3 (registered)
    // Output: sr_v0 .. sr_v3
    wire [63:0] sr_a0 = v0 + v1;
    wire [63:0] sr_a1 = rotl64(v1, 13);
    wire [63:0] sr_b1 = sr_a1 ^ sr_a0;
    wire [63:0] sr_b0 = rotl64(sr_a0, 32);

    wire [63:0] sr_a2 = v2 + v3;
    wire [63:0] sr_a3 = rotl64(v3, 16);
    wire [63:0] sr_b3 = sr_a3 ^ sr_a2;

    wire [63:0] sr_c0 = sr_b0 + sr_b3;
    wire [63:0] sr_c3 = rotl64(sr_b3, 21);
    wire [63:0] sr_v3 = sr_c3 ^ sr_c0;

    wire [63:0] sr_c2 = sr_a2 + sr_b1;
    wire [63:0] sr_c1 = rotl64(sr_b1, 17);
    wire [63:0] sr_v1 = sr_c1 ^ sr_c2;
    wire [63:0] sr_v2 = rotl64(sr_c2, 32);
    wire [63:0] sr_v0 = sr_c0;

    // ── Padding block (length in byte 7, rest zero) ─────────────────────
    wire [63:0] pad_block = {msg_len, 56'd0};

    // ── Byte-insert helper: OR a byte into the accumulator ──────────────
    wire [63:0] byte_shifted = {56'd0, msg_byte} << {byte_cnt, 3'b000};
    wire [63:0] accum_next   = msg_block | byte_shifted;

    // ── Main FSM ────────────────────────────────────────────────────────
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= S_IDLE;
            v0             <= 64'd0;
            v1             <= 64'd0;
            v2             <= 64'd0;
            v3             <= 64'd0;
            msg_block      <= 64'd0;
            current_block  <= 64'd0;
            byte_cnt       <= 3'd0;
            msg_len        <= 8'd0;
            round_cnt      <= 3'd0;
            hash_out       <= 64'd0;
            hash_valid     <= 1'b0;
            busy           <= 1'b0;
            finalizing     <= 1'b0;
            need_pad_block <= 1'b0;
        end else begin
            hash_valid <= 1'b0;  // default: single-cycle pulse

            case (state)
            // ─────────────────────────────────────────────────────────
            S_IDLE: begin
                if (start) begin
                    v0             <= key[63:0]   ^ CONST_0;
                    v1             <= key[127:64] ^ CONST_1;
                    v2             <= key[63:0]   ^ CONST_2;
                    v3             <= key[127:64] ^ CONST_3;
                    msg_block      <= 64'd0;
                    current_block  <= 64'd0;
                    byte_cnt       <= 3'd0;
                    msg_len        <= 8'd0;
                    round_cnt      <= 3'd0;
                    busy           <= 1'b1;
                    finalizing     <= 1'b0;
                    need_pad_block <= 1'b0;
                    state          <= S_ACCUM;
                end
            end

            // ─────────────────────────────────────────────────────────
            S_ACCUM: begin
                if (msg_valid && msg_last && byte_cnt == 3'd7) begin
                    // Last byte fills a complete 8-byte block — compress
                    // normally, then we still need a separate padding block.
                    current_block  <= msg_block | ({56'd0, msg_byte} << 56);
                    v3             <= v3 ^ (msg_block | ({56'd0, msg_byte} << 56));
                    msg_len        <= msg_len + 8'd1;
                    msg_block      <= 64'd0;
                    byte_cnt       <= 3'd0;
                    need_pad_block <= 1'b1;
                    finalizing     <= 1'b0;
                    round_cnt      <= 3'd0;
                    state          <= S_COMPRESS;

                end else if (msg_valid && msg_last) begin
                    // Last byte into a partial block — pad and compress.
                    // Byte 7 = total length (msg_len + 1).
                    current_block  <= (accum_next & 64'h00FFFFFFFFFFFFFF)
                                    | ({msg_len + 8'd1, 56'd0});
                    v3             <= v3 ^ ((accum_next & 64'h00FFFFFFFFFFFFFF)
                                    | ({msg_len + 8'd1, 56'd0}));
                    msg_len        <= msg_len + 8'd1;
                    finalizing     <= 1'b1;
                    need_pad_block <= 1'b0;
                    round_cnt      <= 3'd0;
                    state          <= S_COMPRESS;

                end else if (msg_valid && byte_cnt == 3'd7) begin
                    // Full 8-byte block (not last) — compress it.
                    current_block  <= msg_block | ({56'd0, msg_byte} << 56);
                    v3             <= v3 ^ (msg_block | ({56'd0, msg_byte} << 56));
                    msg_len        <= msg_len + 8'd1;
                    msg_block      <= 64'd0;
                    byte_cnt       <= 3'd0;
                    round_cnt      <= 3'd0;
                    state          <= S_COMPRESS;

                end else if (msg_valid) begin
                    // Accumulate byte (not full, not last).
                    msg_block <= accum_next;
                    byte_cnt  <= byte_cnt + 3'd1;
                    msg_len   <= msg_len + 8'd1;

                end else if (msg_last) begin
                    // End-of-message without a new byte (e.g. empty msg or
                    // last byte already sent on a block boundary).
                    current_block  <= (msg_block & 64'h00FFFFFFFFFFFFFF)
                                    | ({msg_len, 56'd0});
                    v3             <= v3 ^ ((msg_block & 64'h00FFFFFFFFFFFFFF)
                                    | ({msg_len, 56'd0}));
                    finalizing     <= 1'b1;
                    need_pad_block <= 1'b0;
                    round_cnt      <= 3'd0;
                    state          <= S_COMPRESS;
                end
            end

            // ─────────────────────────────────────────────────────────
            // 2 SipRounds per message block.
            // After round 1:  v0 ^= m.  Then decide next phase.
            S_COMPRESS: begin
                if (round_cnt == 3'd1) begin
                    // Second round done — apply v0 ^= m
                    v0 <= sr_v0 ^ current_block;
                    v1 <= sr_v1;

                    if (need_pad_block) begin
                        // Need one more block: the padding block
                        v2             <= sr_v2;
                        v3             <= sr_v3;
                        need_pad_block <= 1'b0;
                        state          <= S_PAD_SETUP;
                    end else if (finalizing) begin
                        // Last block compressed — finalize
                        v2    <= sr_v2;
                        v3    <= sr_v3;
                        state <= S_FIN_XOR;
                    end else begin
                        // More message bytes to come
                        v2    <= sr_v2;
                        v3    <= sr_v3;
                        state <= S_ACCUM;
                    end
                end else begin
                    // First round
                    v0        <= sr_v0;
                    v1        <= sr_v1;
                    v2        <= sr_v2;
                    v3        <= sr_v3;
                    round_cnt <= 3'd1;
                end
            end

            // ─────────────────────────────────────────────────────────
            // Set up extra padding block when message was 8-byte aligned.
            // pad_block = {msg_len, 56'd0}
            S_PAD_SETUP: begin
                current_block <= pad_block;
                v3            <= v3 ^ pad_block;
                finalizing    <= 1'b1;
                round_cnt     <= 3'd0;
                state         <= S_COMPRESS;
            end

            // ─────────────────────────────────────────────────────────
            // v2 ^= 0xFF before finalization rounds
            S_FIN_XOR: begin
                v2        <= v2 ^ 64'h00000000000000FF;
                round_cnt <= 3'd0;
                state     <= S_FINALIZE;
            end

            // ─────────────────────────────────────────────────────────
            // 4 finalization SipRounds
            S_FINALIZE: begin
                v0 <= sr_v0;
                v1 <= sr_v1;
                v2 <= sr_v2;
                v3 <= sr_v3;

                if (round_cnt == 3'd3) begin
                    hash_out   <= sr_v0 ^ sr_v1 ^ sr_v2 ^ sr_v3;
                    hash_valid <= 1'b1;
                    busy       <= 1'b0;
                    state      <= S_DONE;
                end else begin
                    round_cnt <= round_cnt + 3'd1;
                end
            end

            // ─────────────────────────────────────────────────────────
            S_DONE: begin
                state <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule
