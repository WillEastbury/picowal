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
#   [31:28] opcode class (4 bits = 16 classes)
#   [27:24] opcode variant (4 bits = 16 per class)
#   [23:20] dst register (4 bits = R0-R15)
#   [19:16] src1 register (4 bits)
#   [15:0]  immediate / src2 / card address (16 bits)
#
# Card addresses use the numeric namespace:
#   Bits [15:10] = card (0-63 in 16-bit mode, extended via SETBASE)
#   Bits [9:5]   = folder (0-31)
#   Bits [4:0]   = file (0-31)
#
# For larger namespaces, SETBASE sets a 32-bit base that subsequent
# FETCH/STORE instructions offset from.
# ═══════════════════════════════════════════════════════════════════════

# Opcode classes (4 bits, [31:28])
OP_CLASS_LOAD    = 0x0   # Load immediate / set registers
OP_CLASS_FETCH   = 0x1   # Read card from storage into register
OP_CLASS_STORE   = 0x2   # Write register content to card in storage
OP_CLASS_EMIT    = 0x3   # Output register to TCP stream
OP_CLASS_ALU     = 0x4   # Arithmetic/logic (add, sub, cmp, and, or, xor)
OP_CLASS_BRANCH  = 0x5   # Conditional/unconditional branch
OP_CLASS_CALL    = 0x6   # Call another card as subroutine (push return addr)
OP_CLASS_STRING  = 0x7   # String ops (concat, template, substr, find)
OP_CLASS_FILTER  = 0x8   # Predicate filter (match, range, regex-lite)
OP_CLASS_ITER    = 0x9   # Iterator (scan range of cards, yield each)
OP_CLASS_CRYPTO  = 0xA   # Hash, HMAC, simple XOR cipher
OP_CLASS_NET     = 0xB   # Network: HTTP header emit, status code, content-type
OP_CLASS_DSP     = 0xC   # Vector ops (dot product, distance — uses DSP MACs)
OP_CLASS_SYS     = 0xD   # System: YIELD, HALT, SLEEP, GETTIME, SETBASE
OP_CLASS_META    = 0xE   # Card metadata: SIZE, EXISTS, CREATED, MODIFIED
OP_CLASS_RSVD    = 0xF   # Reserved for future use

# ─── Opcode variants within each class ───────────────────────────────

# LOAD class (0x0)
OP_LOAD_IMM16    = 0x00  # Rd = zero_extend(imm16)
OP_LOAD_HIGH     = 0x01  # Rd[31:16] = imm16 (set upper half)
OP_LOAD_MOV      = 0x02  # Rd = Rs1
OP_LOAD_ZERO     = 0x03  # Rd = 0
OP_LOAD_LEN      = 0x04  # Rd = length(Rs1) (bytes in card/string)

# FETCH class (0x1)
OP_FETCH_CARD    = 0x10  # Rd = read_card(imm16)  — card addr in imm16
OP_FETCH_REG     = 0x11  # Rd = read_card(Rs1)    — card addr in register
OP_FETCH_FIELD   = 0x12  # Rd = read_field(Rs1, imm16) — field offset
OP_FETCH_RANGE   = 0x13  # Rd = read_card_range(Rs1, Rs2) — byte range
OP_FETCH_INDEX   = 0x14  # Rd = index_lookup(Rs1) — B-tree key lookup

# STORE class (0x2)
OP_STORE_CARD    = 0x20  # write_card(imm16, Rs1)   — store Rs1 at addr
OP_STORE_REG     = 0x21  # write_card(Rd, Rs1)      — addr in Rd
OP_STORE_APPEND  = 0x22  # append_to_card(imm16, Rs1) — WAL append
OP_STORE_FIELD   = 0x23  # write_field(Rd, imm16, Rs1) — field update

# EMIT class (0x3)
OP_EMIT_REG      = 0x30  # emit(Rs1) — whole register content to TCP
OP_EMIT_LITERAL  = 0x31  # emit(imm16 bytes from instruction stream)
OP_EMIT_FIELD    = 0x32  # emit field from Rs1 at offset imm16
OP_EMIT_TEMPLATE = 0x33  # emit Rs1 with {{}} substitutions from Rs2
OP_EMIT_CHUNK    = 0x34  # emit with HTTP chunked encoding wrapper
OP_EMIT_PIPE     = 0x35  # fetch card(imm16) AND emit directly (zero-copy, no register)
OP_EMIT_PIPE_REG = 0x36  # fetch card(Rs1) AND emit directly (zero-copy)
OP_EMIT_PIPE_TPL = 0x37  # fetch card(imm16) as template, substitute from Rs1, emit

# ALU class (0x4)
OP_ALU_ADD       = 0x40  # Rd = Rs1 + imm16
OP_ALU_SUB       = 0x41  # Rd = Rs1 - imm16
OP_ALU_MUL       = 0x42  # Rd = Rs1 * imm16
OP_ALU_DIV       = 0x43  # Rd = Rs1 / imm16
OP_ALU_MOD       = 0x44  # Rd = Rs1 % imm16
OP_ALU_AND       = 0x45  # Rd = Rs1 & Rs2
OP_ALU_OR        = 0x46  # Rd = Rs1 | Rs2
OP_ALU_XOR       = 0x47  # Rd = Rs1 ^ Rs2
OP_ALU_SHL       = 0x48  # Rd = Rs1 << imm16
OP_ALU_SHR       = 0x49  # Rd = Rs1 >> imm16
OP_ALU_CMP       = 0x4A  # flags = compare(Rs1, Rs2)
OP_ALU_NOT       = 0x4B  # Rd = ~Rs1

# BRANCH class (0x5)
OP_BRANCH_ALWAYS = 0x50  # PC += sign_extend(imm16)
OP_BRANCH_EQ     = 0x51  # if flags.EQ: PC += imm16
OP_BRANCH_NE     = 0x52  # if !flags.EQ: PC += imm16
OP_BRANCH_LT     = 0x53  # if flags.LT: PC += imm16
OP_BRANCH_GT     = 0x54  # if flags.GT: PC += imm16
OP_BRANCH_LE     = 0x55  # if flags.LE: PC += imm16
OP_BRANCH_GE     = 0x56  # if flags.GE: PC += imm16

# CALL class (0x6)
OP_CALL_CARD     = 0x60  # push PC; PC = card(imm16) instruction 0
OP_CALL_REG      = 0x61  # push PC; PC = card(Rs1) instruction 0
OP_CALL_RETURN   = 0x62  # pop PC (return from subroutine card)
OP_CALL_TAIL     = 0x63  # PC = card(imm16) without push (tail call)

# STRING class (0x7)
OP_STR_CONCAT    = 0x70  # Rd = concat(Rs1, Rs2)
OP_STR_SUBSTR    = 0x71  # Rd = substr(Rs1, offset=Rs2, len=imm16)
OP_STR_FIND      = 0x72  # Rd = index_of(Rs1, Rs2) (-1 if not found)
OP_STR_REPLACE   = 0x73  # Rd = replace(Rs1, pattern=Rs2, with=Rd)
OP_STR_SPLIT     = 0x74  # Rd = split(Rs1, delim=Rs2) -> card array
OP_STR_ITOA      = 0x75  # Rd = int_to_string(Rs1)
OP_STR_ATOI      = 0x76  # Rd = string_to_int(Rs1)
OP_STR_UPPER     = 0x77  # Rd = uppercase(Rs1)
OP_STR_LOWER     = 0x78  # Rd = lowercase(Rs1)

# FILTER class (0x8)
OP_FILTER_EQ     = 0x80  # Rd = (Rs1 == Rs2) ? 1 : 0
OP_FILTER_RANGE  = 0x81  # Rd = (Rs1 >= Rs2 && Rs1 <= Rd) ? 1 : 0
OP_FILTER_MATCH  = 0x82  # Rd = glob_match(Rs1, pattern=Rs2)
OP_FILTER_AND    = 0x83  # Rd = Rs1 && Rs2 (predicate combine)
OP_FILTER_OR     = 0x84  # Rd = Rs1 || Rs2
OP_FILTER_NOT    = 0x85  # Rd = !Rs1

# ITER class (0x9)
OP_ITER_RANGE    = 0x90  # Rd = iterator(card_start=Rs1, card_end=Rs2)
OP_ITER_NEXT     = 0x91  # Rd = next(Rs1); flags.EOF if done
OP_ITER_FILTER   = 0x92  # Rd = filter_iter(Rs1, predicate_card=Rs2)
OP_ITER_MAP      = 0x93  # Rd = map_iter(Rs1, transform_card=Rs2)
OP_ITER_COLLECT  = 0x94  # Rd = collect(Rs1) -> new card with all results
OP_ITER_COUNT    = 0x95  # Rd = count(Rs1) (consume iterator, return count)
OP_ITER_YIELD    = 0x96  # yield Rd to caller (streaming result)

# CRYPTO class (0xA)
OP_CRYPTO_HASH   = 0xA0  # Rd = sha256(Rs1)
OP_CRYPTO_HMAC   = 0xA1  # Rd = hmac_sha256(key=Rs1, data=Rs2)
OP_CRYPTO_XOR    = 0xA2  # Rd = xor_cipher(Rs1, key=Rs2)
OP_CRYPTO_CRC32  = 0xA3  # Rd = crc32(Rs1)

# NET class (0xB)
OP_NET_STATUS    = 0xB0  # emit HTTP status line (imm16 = status code)
OP_NET_HEADER    = 0xB1  # emit HTTP header (Rs1=name, Rs2=value)
OP_NET_CTYPE     = 0xB2  # emit Content-Type (imm16 = enum: html/json/text/bin)
OP_NET_BODY      = 0xB3  # emit end-of-headers, begin body
OP_NET_CLOSE     = 0xB4  # close TCP connection after response
OP_NET_REDIRECT  = 0xB5  # emit 302 redirect to URL in Rs1

# DSP class (0xC) — uses FPGA DSP MACs where available
OP_DSP_DOT       = 0xC0  # Rd = dot_product(Rs1, Rs2)
OP_DSP_DIST      = 0xC1  # Rd = euclidean_distance(Rs1, Rs2)
OP_DSP_COSINE    = 0xC2  # Rd = cosine_similarity(Rs1, Rs2)
OP_DSP_TOPK      = 0xC3  # Rd = top_k_nearest(Rs1, k=imm16)
OP_DSP_SUM       = 0xC4  # Rd = sum(Rs1) (vector elements)
OP_DSP_SCALE     = 0xC5  # Rd = Rs1 * imm16 (scalar multiply)

# SYS class (0xD)
OP_SYS_HALT      = 0xD0  # stop execution, return result to caller
OP_SYS_YIELD     = 0xD1  # yield timeslice, resume next cycle
OP_SYS_SLEEP     = 0xD2  # sleep imm16 milliseconds
OP_SYS_TIME      = 0xD3  # Rd = current_time_ms
OP_SYS_SETBASE   = 0xD4  # set 32-bit card address base from Rs1
OP_SYS_GETCONN   = 0xD5  # Rd = connection_id (which TCP socket triggered this)
OP_SYS_LOG       = 0xD6  # log Rs1 to debug output (not to TCP)
OP_SYS_RANDOM    = 0xD7  # Rd = random(0..imm16)

# META class (0xE) — card metadata queries
OP_META_EXISTS   = 0xE0  # Rd = card_exists(imm16) ? 1 : 0
OP_META_SIZE     = 0xE1  # Rd = card_size_bytes(imm16)
OP_META_CREATED  = 0xE2  # Rd = card_created_timestamp(imm16)
OP_META_MODIFIED = 0xE3  # Rd = card_modified_timestamp(imm16)
OP_META_TYPE     = 0xE4  # Rd = card_type(imm16) (data/script/template/index)
OP_META_COUNT    = 0xE5  # Rd = count_cards_in_folder(imm16)


# ═══════════════════════════════════════════════════════════════════════
# Assembler / Disassembler
# ═══════════════════════════════════════════════════════════════════════

def encode_instruction(opcode, rd=0, rs1=0, imm16=0):
    """Encode a 32-bit PicoScript instruction."""
    op_class = (opcode >> 4) & 0xF
    op_variant = opcode & 0xF
    word = (op_class << 28) | (op_variant << 24) | (rd << 20) | (rs1 << 16) | (imm16 & 0xFFFF)
    return word


def decode_instruction(word):
    """Decode a 32-bit PicoScript instruction."""
    op_class = (word >> 28) & 0xF
    op_variant = (word >> 24) & 0xF
    rd = (word >> 20) & 0xF
    rs1 = (word >> 16) & 0xF
    imm16 = word & 0xFFFF
    opcode = (op_class << 4) | op_variant
    return {"opcode": opcode, "rd": rd, "rs1": rs1, "imm16": imm16}


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
    OP_LOAD_IMM16: "LOAD.IMM",    OP_LOAD_HIGH: "LOAD.HI",
    OP_LOAD_MOV: "MOV",           OP_LOAD_ZERO: "ZERO",
    OP_LOAD_LEN: "LEN",
    OP_FETCH_CARD: "FETCH",       OP_FETCH_REG: "FETCH.R",
    OP_FETCH_FIELD: "FETCH.FLD",  OP_FETCH_RANGE: "FETCH.RNG",
    OP_FETCH_INDEX: "FETCH.IDX",
    OP_STORE_CARD: "STORE",       OP_STORE_REG: "STORE.R",
    OP_STORE_APPEND: "STORE.APP", OP_STORE_FIELD: "STORE.FLD",
    OP_EMIT_REG: "EMIT",         OP_EMIT_LITERAL: "EMIT.LIT",
    OP_EMIT_FIELD: "EMIT.FLD",   OP_EMIT_TEMPLATE: "EMIT.TPL",
    OP_EMIT_CHUNK: "EMIT.CHK",   OP_EMIT_PIPE: "PIPE",
    OP_EMIT_PIPE_REG: "PIPE.R",  OP_EMIT_PIPE_TPL: "PIPE.TPL",
    OP_ALU_ADD: "ADD",  OP_ALU_SUB: "SUB",  OP_ALU_MUL: "MUL",
    OP_ALU_DIV: "DIV",  OP_ALU_MOD: "MOD",  OP_ALU_CMP: "CMP",
    OP_BRANCH_ALWAYS: "JMP",      OP_BRANCH_EQ: "JEQ",
    OP_BRANCH_NE: "JNE",         OP_BRANCH_LT: "JLT",
    OP_CALL_CARD: "CALL",        OP_CALL_RETURN: "RET",
    OP_CALL_TAIL: "TAIL",
    OP_STR_CONCAT: "CONCAT",     OP_STR_FIND: "FIND",
    OP_STR_REPLACE: "REPLACE",   OP_STR_ITOA: "ITOA",
    OP_ITER_RANGE: "ITER.RNG",   OP_ITER_NEXT: "ITER.NXT",
    OP_ITER_YIELD: "YIELD",
    OP_NET_STATUS: "HTTP.STATUS", OP_NET_HEADER: "HTTP.HDR",
    OP_NET_CTYPE: "HTTP.CTYPE",  OP_NET_BODY: "HTTP.BODY",
    OP_NET_CLOSE: "HTTP.CLOSE",
    OP_DSP_DOT: "DOT",           OP_DSP_COSINE: "COSINE",
    OP_DSP_TOPK: "TOPK",
    OP_SYS_HALT: "HALT",         OP_SYS_YIELD: "SYS.YIELD",
    OP_SYS_TIME: "TIME",         OP_SYS_SETBASE: "SETBASE",
    OP_META_EXISTS: "EXISTS",    OP_META_SIZE: "SIZE",
}


# ═══════════════════════════════════════════════════════════════════════
# Example programs (as card bytecode)
# ═══════════════════════════════════════════════════════════════════════

def example_hello_world():
    """Card that serves 'Hello World' as HTTP response."""
    return [
        encode_instruction(OP_NET_STATUS, imm16=200),          # HTTP 200 OK
        encode_instruction(OP_NET_CTYPE, imm16=0),             # Content-Type: text/html
        encode_instruction(OP_NET_BODY),                       # end headers
        encode_instruction(OP_EMIT_PIPE, imm16=encode_card_addr(1, 0, 1)),  # PIPE card 1/0/1 direct to TCP
        encode_instruction(OP_SYS_HALT),                       # done
    ]


def example_dynamic_page():
    """Card that renders a user profile page with data from DB."""
    return [
        encode_instruction(OP_NET_STATUS, imm16=200),
        encode_instruction(OP_NET_CTYPE, imm16=0),             # text/html
        encode_instruction(OP_NET_BODY),
        # Load user data (card addr from request parameter in R15)
        encode_instruction(OP_FETCH_REG, rd=1, rs1=15),        # R1 = user record
        # PIPE.TPL: fetch template card 5/0/1, substitute {{fields}} from R1, emit
        encode_instruction(OP_EMIT_PIPE_TPL, rs1=1, imm16=encode_card_addr(5, 0, 1)),
        encode_instruction(OP_SYS_HALT),
    ]


def example_api_endpoint():
    """Card that implements a JSON API: GET /api/users/{id}."""
    return [
        # R15 = request context (card addr of the requested resource)
        encode_instruction(OP_FETCH_REG, rd=0, rs1=15),        # R0 = fetch user card
        encode_instruction(OP_META_EXISTS, rd=1, imm16=0),     # check if fetch succeeded
        # Branch: if not found, return 404
        encode_instruction(OP_ALU_CMP, rd=1, rs1=1, imm16=0), # cmp R1, 0
        encode_instruction(OP_BRANCH_EQ, imm16=4),             # if R1==0, skip to 404
        # 200 OK path
        encode_instruction(OP_NET_STATUS, imm16=200),
        encode_instruction(OP_NET_CTYPE, imm16=1),             # application/json
        encode_instruction(OP_NET_BODY),
        encode_instruction(OP_EMIT_REG, rs1=0),                # emit JSON card content
        encode_instruction(OP_SYS_HALT),
        # 404 path
        encode_instruction(OP_NET_STATUS, imm16=404),
        encode_instruction(OP_NET_CTYPE, imm16=1),
        encode_instruction(OP_NET_BODY),
        encode_instruction(OP_EMIT_LITERAL, imm16=0),          # emit {"error":"not found"}
        encode_instruction(OP_SYS_HALT),
    ]


def example_vector_search():
    """Card that does vector similarity search (uses DSP MACs on midi/maxi)."""
    return [
        # R15 = query vector (from request body)
        encode_instruction(OP_FETCH_CARD, rd=0, imm16=encode_card_addr(10, 0, 0)),  # R0 = vector index
        encode_instruction(OP_DSP_TOPK, rd=1, rs1=0, imm16=10),  # R1 = top-10 nearest
        encode_instruction(OP_NET_STATUS, imm16=200),
        encode_instruction(OP_NET_CTYPE, imm16=1),             # application/json
        encode_instruction(OP_NET_BODY),
        encode_instruction(OP_EMIT_REG, rs1=1),                # emit results
        encode_instruction(OP_SYS_HALT),
    ]


# ═══════════════════════════════════════════════════════════════════════
# Card type markers (first 4 bytes of a card identify its type)
# ═══════════════════════════════════════════════════════════════════════

CARD_MAGIC_DATA     = 0x50574400  # "PWD\0" — raw data card
CARD_MAGIC_SCRIPT   = 0x50575300  # "PWS\0" — PicoScript bytecode
CARD_MAGIC_TEMPLATE = 0x50575400  # "PWT\0" — template (HTML + embedded fetch)
CARD_MAGIC_INDEX    = 0x50574900  # "PWI\0" — B-tree index node
CARD_MAGIC_VECTOR   = 0x50575600  # "PWV\0" — vector embedding data


# ═══════════════════════════════════════════════════════════════════════
# Execution model
# ═══════════════════════════════════════════════════════════════════════

"""
Execution context (per connection):
  - PC: current instruction index within the active card
  - Registers: R0-R14 general purpose, R15 = request context
  - Call stack: up to 8 deep (card addr + PC pairs)
  - Flags: EQ, LT, GT, EOF (set by CMP and ITER.NEXT)
  - Connection ID: which TCP socket this execution serves
  - Cycle budget: max instructions before forced YIELD (fairness)

Scheduling:
  - Each TCP connection maps to one execution context
  - Contexts are round-robin scheduled across query lanes (FPGA)
    or cooperative multitasked (MCU tiers)
  - YIELD voluntarily returns timeslice
  - Cycle budget (default 1024 instructions) forces YIELD if exceeded
  - FETCH from NVMe suspends context until DMA completes (non-blocking)

Memory model:
  - Registers hold either:
    a) A 32-bit scalar (integer, address, boolean)
    b) A reference to a card buffer (in SRAM/PSRAM page cache)
  - No pointer arithmetic — only card-level granularity
  - Card buffers are reference-counted, freed when no register holds them
  - Maximum card size: 64KB (fits in one SRAM bank window)

Security:
  - No way to access raw memory — only cards via FETCH/STORE
  - Card permissions: read-only / read-write / execute
  - Scripts can only CALL other cards marked as executable
  - STORE to read-only cards raises a fault (connection closed)
  - No self-modifying code (instruction stream is read-only during execution)
"""


# ═══════════════════════════════════════════════════════════════════════
# Content-Type enum (for HTTP.CTYPE instruction)
# ═══════════════════════════════════════════════════════════════════════

CONTENT_TYPES = {
    0: "text/html; charset=utf-8",
    1: "application/json",
    2: "text/plain; charset=utf-8",
    3: "application/octet-stream",
    4: "text/css",
    5: "application/javascript",
    6: "image/png",
    7: "image/jpeg",
    8: "image/svg+xml",
    9: "application/xml",
}


# ═══════════════════════════════════════════════════════════════════════
# Pretty-print / disassemble
# ═══════════════════════════════════════════════════════════════════════

def disassemble(program, base_addr=0):
    """Disassemble a list of instruction words."""
    lines = []
    for i, word in enumerate(program):
        d = decode_instruction(word)
        name = OPCODE_NAMES.get(d["opcode"], f"OP_{d['opcode']:02X}")
        addr = base_addr + i
        # Format based on instruction type
        if d["opcode"] in (OP_FETCH_CARD, OP_STORE_CARD, OP_EMIT_PIPE):
            c, f, fi = decode_card_addr(d["imm16"])
            operands = f"R{d['rd']}, [{c}/{f}/{fi}]"
        elif d["opcode"] == OP_EMIT_PIPE_TPL:
            c, f, fi = decode_card_addr(d["imm16"])
            operands = f"[{c}/{f}/{fi}], data=R{d['rs1']}"
        elif d["opcode"] in (OP_EMIT_REG, OP_FETCH_REG):
            operands = f"R{d['rs1']}"
        elif d["opcode"] in (OP_NET_STATUS,):
            operands = f"{d['imm16']}"
        elif d["opcode"] in (OP_BRANCH_ALWAYS, OP_BRANCH_EQ, OP_BRANCH_NE, OP_BRANCH_LT):
            target = addr + 1 + d["imm16"]
            operands = f"-> @{target}"
        elif d["opcode"] in (OP_SYS_HALT, OP_NET_BODY):
            operands = ""
        else:
            operands = f"R{d['rd']}, R{d['rs1']}, {d['imm16']}"
        lines.append(f"  {addr:4d}: {name:14s} {operands}")
    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════════════
# Main (demo)
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("PicoScript ISA v1.0")
    print("=" * 60)
    print(f"  Instruction width:  32 bits (fixed)")
    print(f"  Registers:          16 (R0-R15)")
    print(f"  Opcode classes:     15 (+ 1 reserved)")
    print(f"  Total opcodes:      {len(OPCODE_NAMES)} defined")
    print(f"  Card address:       16-bit inline (64 cards × 32 folders × 32 files)")
    print(f"  Extended address:   32-bit via SETBASE + offset")
    print(f"  Max card size:      64KB")
    print(f"  Call depth:         8 levels")
    print(f"  Cycle budget:       1024 instructions / yield")
    print()

    print("Example: Hello World HTTP server (card 2/0/0)")
    print("-" * 60)
    prog = example_hello_world()
    print(disassemble(prog))
    print(f"  ({len(prog)} instructions, {len(prog)*4} bytes)")
    print()

    print("Example: Dynamic page with template (card 2/0/1)")
    print("-" * 60)
    prog = example_dynamic_page()
    print(disassemble(prog))
    print(f"  ({len(prog)} instructions, {len(prog)*4} bytes)")
    print()

    print("Example: JSON API endpoint (card 2/0/2)")
    print("-" * 60)
    prog = example_api_endpoint()
    print(disassemble(prog))
    print(f"  ({len(prog)} instructions, {len(prog)*4} bytes)")
    print()

    print("Example: Vector similarity search (card 2/0/3)")
    print("-" * 60)
    prog = example_vector_search()
    print(disassemble(prog))
    print(f"  ({len(prog)} instructions, {len(prog)*4} bytes)")
    print()

    print("Execution model:")
    print("-" * 60)
    print("  TCP request arrives -> dispatch to query lane")
    print("  URL path maps to card address (e.g. /2/0/1 -> card 2/0/1)")
    print("  If card magic == PWS (script): execute PicoScript")
    print("  If card magic == PWD (data):   stream raw bytes to TCP")
    print("  If card magic == PWT (template): auto-substitute + emit")
    print()
    print("  On FPGA (midi/maxi): each query lane has its own decoder")
    print("    -> 44 PicoScript programs execute simultaneously")
    print("    -> FETCH suspends lane until DMA completes (other lanes continue)")
    print()
    print("  On MCU (pico/mini): cooperative scheduling")
    print("    -> yield after cycle budget or FETCH (I/O wait)")
    print("    -> context switch to next connection's program")
