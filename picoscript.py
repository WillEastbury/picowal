#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""picoscript.py -- PicoScript Instruction Set Architecture

PicoScript: executable cards for PicoWAL.

Core insight: the TCP state machine and query engine are already jump tables.
PicoScript extends the jump table so that a CARD can contain instructions
that the engine executes directly from storage. No OS. No filesystem.
Storage IS compute.

A card containing PicoScript bytecode is indistinguishable from a "program".
The engine loads the card, walks the instruction stream, and the jump table
dispatches each opcode. Outputs go directly to the TCP stream (or to other cards).

Example: a webpage renderer is card #5000 containing:
  FETCH 5001        ; load HTML template card into register
  FETCH 100/1/42   ; load user record (card 100, folder 1, file 42)
  TEMPLATE R0 R1   ; substitute {{name}} etc. from R1 into R0
  EMIT R0          ; stream result to TCP connection
  HALT             ; done

This works identically across all tiers:
  - Pico:  RP2354B firmware interprets PicoScript from SD card
  - Mini:  RP2354B firmware interprets from eMMC (faster I/O)
  - Midi:  FPGA executes PicoScript in hardware (combinatorial decode)
  - Maxi:  FPGA hardware + RK3588 can JIT-compile hot PicoScript cards

Design principles:
  1. Fixed-width instructions (32-bit) — easy to decode in hardware
  2. Register file: 16 registers (R0-R15), each holds a card reference or scalar
  3. No pointers, no malloc — only card references and fixed-size values
  4. Control flow: BRANCH, CALL (to another card), RETURN
  5. I/O: FETCH (read card), EMIT (write to TCP), STORE (write card)
  6. Deterministic: no unbounded loops without YIELD (cooperative scheduling)
"""

# ═══════════════════════════════════════════════════════════════════════
# Instruction encoding: 32-bit fixed width
#
#   [31:28] opcode (4 bits = 16 core instructions)
#   [27:24] dst register (4 bits = R0-R15)
#   [23:20] src1 register (4 bits)
#   [19:16] src2 register / flags (4 bits)
#   [15:0]  immediate (16 bits, signed or unsigned depending on op)
#
# This is the CORE ISA. 16 instructions. That's it.
# Everything else is built from these or is a card you CALL.
# ═══════════════════════════════════════════════════════════════════════

# The 16 core opcodes (4 bits, [31:28])
OP_NOOP     = 0x0   # No operation (pipeline bubble / alignment)
OP_LOAD     = 0x1   # Rd = card[imm16] — load card from storage into register
OP_SAVE     = 0x2   # card[imm16] = Rs1 — save register to card in storage
OP_PIPE     = 0x3   # card[imm16] -> TCP (fetch + emit, zero copy)
OP_ADD      = 0x4   # Rd = Rs1 + Rs2 (or Rd = Rs1 + imm16)
OP_SUB      = 0x5   # Rd = Rs1 - Rs2 (or Rd = Rs1 - imm16)
OP_MUL      = 0x6   # Rd = Rs1 * Rs2 (or Rd = Rs1 * imm16)
OP_DIV      = 0x7   # Rd = Rs1 / Rs2 (or Rd = Rs1 / imm16)
OP_INC      = 0x8   # Rd = Rd + 1
OP_JUMP     = 0x9   # PC = imm16 (unconditional jump)
OP_BRANCH   = 0xA   # if (Rs1 <cond> Rs2) PC += sign_extend(imm16)
OP_CALL     = 0xB   # push PC; execute card[imm16] as subroutine
OP_RETURN   = 0xC   # pop PC; resume caller
OP_WAIT     = 0xD   # suspend until software interrupt (from another card/connection)
OP_RAISE    = 0xE   # raise software interrupt imm16 (wake WAITing contexts)
OP_DSP      = 0xF   # DSP subops: [19:16] selects MATMUL/SOFTMAX/DOT/etc.

# ═══════════════════════════════════════════════════════════════════════
# Addressing modes (encoded in Rs2 field bits [19:16] for LOAD/SAVE/PIPE)
#
#   Rs2 == 0x0: immediate mode    — card address is imm16
#   Rs2 == 0x1: register indirect — card address is in Rs1 (TURING COMPLETE)
#   Rs2 == 0x2: base+offset       — card address is BASE + imm16
#   Rs2 == 0x3: register+offset   — card address is Rs1 + imm16
#
# Register indirect (mode 0x1) is what makes this Turing complete:
#   you can compute an address, put it in a register, and LOAD from it.
#   Without this, you cannot follow pointers or index arrays.
# ═══════════════════════════════════════════════════════════════════════

ADDR_IMMEDIATE  = 0x0   # card[imm16]
ADDR_REGISTER   = 0x1   # card[Rs1]         <- TURING COMPLETE
ADDR_BASE_OFF   = 0x2   # card[BASE+imm16]
ADDR_REG_OFF    = 0x3   # card[Rs1+imm16]

# DSP sub-operations (encoded in bits [19:16] when opcode == OP_DSP)
DSP_MATMUL    = 0x0   # Rd = matmul(Rs1, Rs2) — matrix multiply via MACs
DSP_SOFTMAX   = 0x1   # Rd = softmax(Rs1) — exp + normalize
DSP_DOT       = 0x2   # Rd = dot(Rs1, Rs2) — dot product
DSP_SCALE     = 0x3   # Rd = Rs1 * imm16 (scalar broadcast multiply)
DSP_RELU      = 0x4   # Rd = max(0, Rs1) element-wise
DSP_NORM      = 0x5   # Rd = layer_norm(Rs1) — normalize vector
DSP_TOPK      = 0x6   # Rd = top_k(Rs1, k=imm16) — k largest elements
DSP_GELU      = 0x7   # Rd = gelu(Rs1) — activation function
DSP_TRANSPOSE = 0x8   # Rd = transpose(Rs1) — matrix T (needed for Q×K^T)
DSP_VADD      = 0x9   # Rd = Rs1 + Rs2 element-wise (residual connections)
DSP_EMBED     = 0xA   # Rd = row(Rs1, imm16) — fetch embedding row N from matrix
DSP_QUANT     = 0xB   # Rd = quantize(Rs1) — float -> INT8 (4× MAC throughput)
DSP_DEQUANT   = 0xC   # Rd = dequantize(Rs1) — INT8 -> float
DSP_MASK      = 0xD   # Rd = mask(Rs1, Rs2) — apply attention mask (causal/padding)
DSP_CONCAT    = 0xE   # Rd = concat(Rs1, Rs2) — join vectors (multi-head merge)
DSP_SPLIT     = 0xF   # Rd = split(Rs1, imm16) — slice at offset (head extraction)

# ═══════════════════════════════════════════════════════════════════════
# Branch condition modes (encoded in Rs2 field when opcode == OP_BRANCH)
# When Rs2 == 0xF, use imm16 as literal compare value instead
# ═══════════════════════════════════════════════════════════════════════

BRANCH_EQ   = 0x0   # branch if Rs1 == Rs2
BRANCH_NE   = 0x1   # branch if Rs1 != Rs2
BRANCH_LT   = 0x2   # branch if Rs1 < Rs2
BRANCH_GT   = 0x3   # branch if Rs1 > Rs2
BRANCH_LE   = 0x4   # branch if Rs1 <= Rs2
BRANCH_GE   = 0x5   # branch if Rs1 >= Rs2
BRANCH_Z    = 0x6   # branch if Rs1 == 0 (zero test)
BRANCH_NZ   = 0x7   # branch if Rs1 != 0 (non-zero)
BRANCH_EOF  = 0x8   # branch if iterator exhausted
BRANCH_ERR  = 0x9   # branch if last op errored

# ═══════════════════════════════════════════════════════════════════════
# That's the whole ISA. 16 opcodes + 16 DSP sub-ops + 10 branch modes.
# 4 addressing modes. Turing complete. AI-accelerated.
#
# PROOF OF TURING COMPLETENESS:
#   1. Unbounded storage: cards are the tape (LOAD.R / SAVE.R = read/write head)
#   2. Conditional branch: BRANCH with 10 condition modes
#   3. Arithmetic: ADD/SUB/MUL/DIV/INC for address computation
#   4. Indirect addressing: LOAD Rd, [Rs1] — follow computed pointers
#   These four together can simulate any Turing machine.
#   (Cards = tape cells, Rs1 = head position, BRANCH = state transitions)
#
# FULL TRANSFORMER FORWARD PASS (GPT-style, single layer):
#
#   ; Card layout: weights stored as data cards
#   ; Card 1000 = W_embed (vocab × d_model matrix)
#   ; Card 1001 = W_q, 1002 = W_k, 1003 = W_v (d_model × d_model)
#   ; Card 1004 = W_o (output projection)
#   ; Card 1005 = W_ff1 (d_model × 4*d_model), 1006 = W_ff2
#   ; Card 1007 = layer_norm weights
#   ; Card 2000+ = scratch space for intermediates
#
#   ; --- Embedding ---
#   LOAD R0, 1000             ; R0 = embedding matrix
#   DSP.EMBED R1, R0, [R15]  ; R1 = embed(input_token) — R15 has token id
#
#   ; --- Self-Attention ---
#   LOAD R2, 1001             ; W_q
#   LOAD R3, 1002             ; W_k
#   LOAD R4, 1003             ; W_v
#   DSP.MATMUL R5, R1, R2    ; Q = input × W_q
#   DSP.MATMUL R6, R1, R3    ; K = input × W_k
#   DSP.MATMUL R7, R1, R4    ; V = input × W_v
#   DSP.TRANSPOSE R8, R6     ; K^T
#   DSP.MATMUL R9, R5, R8    ; scores = Q × K^T
#   DSP.SCALE R9, R9, 0x0B   ; scores / sqrt(d_k) — imm16 encodes scale
#   DSP.MASK R9, R9, R14     ; apply causal mask (R14 = mask card)
#   DSP.SOFTMAX R9, R9       ; attention weights
#   DSP.MATMUL R10, R9, R7   ; attn_out = weights × V
#   LOAD R11, 1004            ; W_o
#   DSP.MATMUL R10, R10, R11 ; projected = attn_out × W_o
#
#   ; --- Residual + LayerNorm ---
#   DSP.VADD R1, R1, R10     ; residual connection
#   DSP.NORM R1, R1          ; layer norm
#
#   ; --- Feed-Forward Network ---
#   LOAD R12, 1005            ; W_ff1
#   LOAD R13, 1006            ; W_ff2
#   DSP.MATMUL R10, R1, R12  ; hidden = input × W_ff1
#   DSP.GELU R10, R10        ; activation
#   DSP.MATMUL R10, R10, R13 ; output = hidden × W_ff2
#
#   ; --- Residual + Output ---
#   DSP.VADD R1, R1, R10     ; residual connection
#   DSP.NORM R1, R1          ; final layer norm
#   SAVE R1, 2000             ; save output to scratch card
#   PIPE 2000                 ; stream result to TCP
#   RETURN
#
# That's a complete transformer layer in 28 instructions (112 bytes).
# The weights live in cards. The compute happens on DSP MACs.
# Upload new weight cards = deploy a different model. Zero recompile.
# ═══════════════════════════════════════════════════════════════════════


# ═══════════════════════════════════════════════════════════════════════
# Assembler / Disassembler
# ═══════════════════════════════════════════════════════════════════════

def encode_instruction(opcode, rd=0, rs1=0, rs2=0, imm16=0):
    """Encode a 32-bit PicoScript instruction.
    
    Format: [31:28]=opcode [27:24]=Rd [23:20]=Rs1 [19:16]=Rs2/mode [15:0]=imm16
    """
    word = (opcode << 28) | (rd << 24) | (rs1 << 20) | (rs2 << 16) | (imm16 & 0xFFFF)
    return word


def decode_instruction(word):
    """Decode a 32-bit PicoScript instruction."""
    opcode = (word >> 28) & 0xF
    rd = (word >> 24) & 0xF
    rs1 = (word >> 20) & 0xF
    rs2 = (word >> 16) & 0xF
    imm16 = word & 0xFFFF
    return {"opcode": opcode, "rd": rd, "rs1": rs1, "rs2": rs2, "imm16": imm16}


def encode_card_addr(card, folder, file):
    """Encode card/folder/file into 16-bit address."""
    assert 0 <= card <= 63, f"card {card} out of range (0-63 in 16-bit mode)"
    assert 0 <= folder <= 31, f"folder {folder} out of range (0-31)"
    assert 0 <= file <= 31, f"file {file} out of range (0-31)"
    return (card << 10) | (folder << 5) | file


def decode_card_addr(addr16):
    """Decode 16-bit address into card/folder/file."""
    card = (addr16 >> 10) & 0x3F
    folder = (addr16 >> 5) & 0x1F
    file = addr16 & 0x1F
    return card, folder, file


# ═══════════════════════════════════════════════════════════════════════
# Opcode name table (for disassembly / debug)
# ═══════════════════════════════════════════════════════════════════════

OPCODE_NAMES = {
    OP_NOOP:   "NOOP",
    OP_LOAD:   "LOAD",
    OP_SAVE:   "SAVE",
    OP_PIPE:   "PIPE",
    OP_ADD:    "ADD",
    OP_SUB:    "SUB",
    OP_MUL:    "MUL",
    OP_DIV:    "DIV",
    OP_INC:    "INC",
    OP_JUMP:   "JUMP",
    OP_BRANCH: "BRANCH",
    OP_CALL:   "CALL",
    OP_RETURN: "RETURN",
    OP_WAIT:   "WAIT",
    OP_RAISE:  "RAISE",
    OP_DSP:    "DSP",
}

DSP_NAMES = {
    DSP_MATMUL: "MATMUL", DSP_SOFTMAX: "SOFTMAX", DSP_DOT: "DOT",
    DSP_SCALE: "SCALE", DSP_RELU: "RELU", DSP_NORM: "NORM",
    DSP_TOPK: "TOPK", DSP_GELU: "GELU", DSP_TRANSPOSE: "TRANSPOSE",
    DSP_VADD: "VADD", DSP_EMBED: "EMBED", DSP_QUANT: "QUANT",
    DSP_DEQUANT: "DEQUANT", DSP_MASK: "MASK", DSP_CONCAT: "CONCAT",
    DSP_SPLIT: "SPLIT",
}

BRANCH_NAMES = {
    BRANCH_EQ: "EQ", BRANCH_NE: "NE", BRANCH_LT: "LT",
    BRANCH_GT: "GT", BRANCH_LE: "LE", BRANCH_GE: "GE",
    BRANCH_Z: "Z", BRANCH_NZ: "NZ", BRANCH_EOF: "EOF",
    BRANCH_ERR: "ERR",
}


# ═══════════════════════════════════════════════════════════════════════
# Card type markers (first 4 bytes of a card identify its type)
# ═══════════════════════════════════════════════════════════════════════

CARD_MAGIC_DATA     = 0x50574400  # "PWD " - raw data card
CARD_MAGIC_SCRIPT   = 0x50575300  # "PWS " - PicoScript bytecode
CARD_MAGIC_TEMPLATE = 0x50575400  # "PWT " - template (HTML + embedded fetch)
CARD_MAGIC_INDEX    = 0x50574900  # "PWI " - B-tree index node
CARD_MAGIC_VECTOR   = 0x50575600  # "PWV " - vector embedding data


# ═══════════════════════════════════════════════════════════════════════
# Example programs using new 16-opcode ISA
# ═══════════════════════════════════════════════════════════════════════

def example_serve_page():
    """Serve a static page: 3 instructions (12 bytes)."""
    return [
        encode_instruction(OP_PIPE, imm16=1001),    # fetch card 1001, stream to TCP
        encode_instruction(OP_RETURN),               # done
    ]


def example_loop_and_filter():
    """Loop over cards 100-109, emit those where field > threshold."""
    return [
        # R0 = counter (start), R1 = end, R2 = threshold
        encode_instruction(OP_ADD, rd=0, rs1=0, imm16=100),   # R0 = 100
        encode_instruction(OP_ADD, rd=1, rs1=0, imm16=110),   # R1 = 110
        encode_instruction(OP_ADD, rd=2, rs1=0, imm16=50),    # R2 = 50 (threshold)
        # loop:
        encode_instruction(OP_LOAD, rd=3, rs2=ADDR_REGISTER, rs1=0),  # R3 = card[R0]
        encode_instruction(OP_BRANCH, rd=3, rs1=2, rs2=BRANCH_LE, imm16=2),  # if R3 <= R2, skip emit
        encode_instruction(OP_PIPE, rd=0, rs2=ADDR_REGISTER, rs1=0),  # PIPE card[R0] to TCP
        # continue:
        encode_instruction(OP_INC, rd=0),                      # R0++
        encode_instruction(OP_BRANCH, rd=0, rs1=1, rs2=BRANCH_LT, imm16=0xFFFC),  # if R0 < R1, jump -4 (loop)
        encode_instruction(OP_RETURN),
    ]


def example_transformer_layer():
    """Single transformer attention layer (28 instructions, 112 bytes)."""
    return [
        # Load weight matrices from cards
        encode_instruction(OP_LOAD, rd=0, imm16=1000),  # R0 = W_embed
        encode_instruction(OP_LOAD, rd=1, imm16=1001),  # R1 = W_q
        encode_instruction(OP_LOAD, rd=2, imm16=1002),  # R2 = W_k
        encode_instruction(OP_LOAD, rd=3, imm16=1003),  # R3 = W_v
        encode_instruction(OP_LOAD, rd=4, imm16=1004),  # R4 = W_o
        encode_instruction(OP_LOAD, rd=14, imm16=1010), # R14 = causal mask
        # Embedding lookup
        encode_instruction(OP_DSP, rd=5, rs1=0, rs2=DSP_EMBED, imm16=0),  # R5 = embed(input)
        # Self-attention: Q, K, V projections
        encode_instruction(OP_DSP, rd=6, rs1=5, rs2=DSP_MATMUL),   # R6 = Q = input * W_q (R1 implicit)
        encode_instruction(OP_DSP, rd=7, rs1=5, rs2=DSP_MATMUL),   # R7 = K = input * W_k
        encode_instruction(OP_DSP, rd=8, rs1=5, rs2=DSP_MATMUL),   # R8 = V = input * W_v
        # Attention scores
        encode_instruction(OP_DSP, rd=9, rs1=7, rs2=DSP_TRANSPOSE),  # R9 = K^T
        encode_instruction(OP_DSP, rd=10, rs1=6, rs2=DSP_MATMUL),    # R10 = Q * K^T
        encode_instruction(OP_DSP, rd=10, rs1=10, rs2=DSP_SCALE, imm16=11),  # /sqrt(d)
        encode_instruction(OP_DSP, rd=10, rs1=10, rs2=DSP_MASK),     # causal mask
        encode_instruction(OP_DSP, rd=10, rs1=10, rs2=DSP_SOFTMAX),  # attention weights
        # Weighted values
        encode_instruction(OP_DSP, rd=11, rs1=10, rs2=DSP_MATMUL),   # attn * V
        encode_instruction(OP_DSP, rd=11, rs1=11, rs2=DSP_MATMUL),   # * W_o
        # Residual + norm
        encode_instruction(OP_DSP, rd=5, rs1=5, rs2=DSP_VADD),       # residual
        encode_instruction(OP_DSP, rd=5, rs1=5, rs2=DSP_NORM),       # layer norm
        # FFN
        encode_instruction(OP_LOAD, rd=12, imm16=1005),               # W_ff1
        encode_instruction(OP_LOAD, rd=13, imm16=1006),               # W_ff2
        encode_instruction(OP_DSP, rd=11, rs1=5, rs2=DSP_MATMUL),    # hidden = x * W_ff1
        encode_instruction(OP_DSP, rd=11, rs1=11, rs2=DSP_GELU),     # activation
        encode_instruction(OP_DSP, rd=11, rs1=11, rs2=DSP_MATMUL),   # output = hidden * W_ff2
        # Final residual + output
        encode_instruction(OP_DSP, rd=5, rs1=5, rs2=DSP_VADD),       # residual
        encode_instruction(OP_DSP, rd=5, rs1=5, rs2=DSP_NORM),       # final norm
        encode_instruction(OP_SAVE, rd=5, imm16=2000),                # save to scratch card
        encode_instruction(OP_PIPE, imm16=2000),                      # emit to TCP
        encode_instruction(OP_RETURN),
    ]


def example_wait_interrupt():
    """Card that waits for a software interrupt then processes."""
    return [
        encode_instruction(OP_WAIT),                   # suspend until RAISE wakes us
        encode_instruction(OP_LOAD, rd=0, rs2=ADDR_REGISTER, rs1=15),  # R0 = card[R15] (event data)
        encode_instruction(OP_PIPE, rd=0, rs2=ADDR_REGISTER, rs1=0),   # emit event card
        encode_instruction(OP_JUMP, imm16=0),          # loop back to WAIT
    ]


# ═══════════════════════════════════════════════════════════════════════
# Disassembler
# ═══════════════════════════════════════════════════════════════════════

def disassemble(program):
    """Disassemble a list of instruction words."""
    lines = []
    for i, word in enumerate(program):
        d = decode_instruction(word)
        op = d["opcode"]
        name = OPCODE_NAMES.get(op, f"OP_{op:X}")

        if op == OP_DSP:
            dsp_name = DSP_NAMES.get(d["rs2"], f"SUB_{d["rs2"]:X}")
            lines.append(f"  {i:3d}: DSP.{dsp_name:10s} R{d["rd"]}, R{d["rs1"]}")
        elif op == OP_BRANCH:
            cond = BRANCH_NAMES.get(d["rs2"], f"?{d["rs2"]}")
            offset = d["imm16"] if d["imm16"] < 0x8000 else d["imm16"] - 0x10000
            lines.append(f"  {i:3d}: BRANCH.{cond:4s}  R{d["rd"]}, R{d["rs1"]}, {offset:+d}")
        elif op == OP_LOAD and d["rs2"] == ADDR_REGISTER:
            lines.append(f"  {i:3d}: LOAD         R{d["rd"]}, [R{d["rs1"]}]")
        elif op == OP_PIPE and d["rs2"] == ADDR_REGISTER:
            lines.append(f"  {i:3d}: PIPE         [R{d["rs1"]}]")
        elif op in (OP_LOAD, OP_SAVE, OP_PIPE, OP_CALL):
            lines.append(f"  {i:3d}: {name:12s} R{d["rd"]}, {d["imm16"]}")
        elif op in (OP_ADD, OP_SUB, OP_MUL, OP_DIV):
            lines.append(f"  {i:3d}: {name:12s} R{d["rd"]}, R{d["rs1"]}, {d["imm16"]}")
        elif op == OP_INC:
            lines.append(f"  {i:3d}: INC          R{d["rd"]}")
        elif op == OP_JUMP:
            lines.append(f"  {i:3d}: JUMP         @{d["imm16"]}")
        elif op in (OP_RETURN, OP_NOOP, OP_WAIT):
            lines.append(f"  {i:3d}: {name}")
        elif op == OP_RAISE:
            lines.append(f"  {i:3d}: RAISE        #{d["imm16"]}")
        else:
            lines.append(f"  {i:3d}: {name:12s} R{d["rd"]}, R{d["rs1"]}, {d["imm16"]}")
    return "
".join(lines)


# ═══════════════════════════════════════════════════════════════════════
# Main (demo)
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("PicoScript ISA v2.0 -- Minimal Turing-Complete + AI-Accelerated")
    print("=" * 65)
    print()
    print("  CORE: 16 opcodes (4-bit decode, single clock on FPGA)")
    print("  " + "-" * 60)
    for op, name in sorted(OPCODE_NAMES.items()):
        print(f"    0x{op:X}  {name}")
    print()
    print("  DSP: 16 sub-operations (neural network acceleration)")
    print("  " + "-" * 60)
    for op, name in sorted(DSP_NAMES.items()):
        print(f"    0x{op:X}  {name}")
    print()
    print("  BRANCH: 10 condition modes")
    print("  " + "-" * 60)
    for op, name in sorted(BRANCH_NAMES.items()):
        print(f"    0x{op:X}  {name}")
    print()
    print("  ADDRESSING: 4 modes (register indirect = Turing complete)")
    print("  " + "-" * 60)
    print("    0x0  IMMEDIATE   card[imm16]")
    print("    0x1  REGISTER    card[Rs1]       <- makes it Turing complete")
    print("    0x2  BASE+OFF    card[BASE+imm16]")
    print("    0x3  REG+OFF     card[Rs1+imm16]")
    print()

    print("Example: Static page server (2 instructions, 8 bytes)")
    print("-" * 65)
    print(disassemble(example_serve_page()))
    print()

    print("Example: Loop + filter (9 instructions, 36 bytes)")
    print("-" * 65)
    print(disassemble(example_loop_and_filter()))
    print()

    print("Example: Transformer layer (29 instructions, 116 bytes)")
    print("-" * 65)
    prog = example_transformer_layer()
    print(disassemble(prog))
    print()

    print("Example: Event-driven (WAIT/RAISE, 4 instructions, 16 bytes)")
    print("-" * 65)
    print(disassemble(example_wait_interrupt()))
    print()

    print("Summary:")
    print("-" * 65)
    print("  Full transformer layer:            116 bytes on storage")
    print("  Upload new model:                  just write new weight cards")
    print("  Turing complete:                   LOAD [Rs1] + BRANCH + INC")
    print("  AI acceleration:                   MATMUL/SOFTMAX on 312 DSP MACs")
    print("  Web server:                        PIPE [card] = zero-copy serve")
    print("  Event-driven:                      WAIT/RAISE = cooperative actors")

