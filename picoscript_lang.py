#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""picoscript_lang.py -- PicoScript Source Language & Compiler

PicoScript source looks like method calls on hardware namespaces:

    Storage.Load(tenant, pack, card, R0);
    Math.Add(R1, R0, 42);
    Flow.Branch(GT, R1, R0, :done);
    Storage.Pipe(tenant, pack, card, Stream.Out);
    :done
    Flow.Return();

Each statement compiles 1:1 to a single 32-bit instruction.
No optimisation. No reordering. What you write is what executes.

Namespaces map directly to FPGA hardware subsystems:

    Storage.*   → SRAM/SD controller
    Thread.*    → Context scheduler
    Math.*      → ALU (combinatorial + soft MUL/DIV)
    Flow.*      → Branch unit + call stack
    Dsp.*       → Soft MAC / ECP5 coprocessor
    Net.*       → HTTP response framer (integrated with HW parser)

Registers: R0-R14 general purpose, R15 = connection context (read-only)
Labels: prefixed with colon (:label)
Streams: Stream.Out (TCP TX), Stream.In (TCP RX)
Conditions: EQ, NE, LT, GT, LE, GE, Z, NZ, EOF, ERR

The compiler is ~200 lines because there's nothing to optimise.
It's a 1:1 text→bytecode translator. That's the whole point.
"""


# ═══════════════════════════════════════════════════════════════════════
# Instruction encoding (matches hardware decoder exactly)
# ═══════════════════════════════════════════════════════════════════════

# Opcodes [31:28] — 4-bit, single clock decode
OP_NOOP   = 0x0
OP_LOAD   = 0x1
OP_SAVE   = 0x2
OP_PIPE   = 0x3
OP_ADD    = 0x4
OP_SUB    = 0x5
OP_MUL    = 0x6
OP_DIV    = 0x7
OP_INC    = 0x8
OP_JUMP   = 0x9
OP_BRANCH = 0xA
OP_CALL   = 0xB
OP_RETURN = 0xC
OP_WAIT   = 0xD
OP_RAISE  = 0xE
OP_DSP    = 0xF

# DSP sub-ops [19:16]
DSP_MATMUL    = 0x0
DSP_SOFTMAX   = 0x1
DSP_DOT       = 0x2
DSP_SCALE     = 0x3
DSP_RELU      = 0x4
DSP_NORM      = 0x5
DSP_TOPK      = 0x6
DSP_GELU      = 0x7
DSP_TRANSPOSE = 0x8
DSP_VADD      = 0x9
DSP_EMBED     = 0xA
DSP_QUANT     = 0xB
DSP_DEQUANT   = 0xC
DSP_MASK      = 0xD
DSP_CONCAT    = 0xE
DSP_SPLIT     = 0xF

# Branch conditions [19:16]
COND_EQ  = 0x0
COND_NE  = 0x1
COND_LT  = 0x2
COND_GT  = 0x3
COND_LE  = 0x4
COND_GE  = 0x5
COND_Z   = 0x6
COND_NZ  = 0x7
COND_EOF = 0x8
COND_ERR = 0x9

# Addressing modes (encoded in Rs2 field when relevant)
ADDR_IMMEDIATE = 0x0
ADDR_REGISTER  = 0x1
ADDR_BASE_OFF  = 0x2
ADDR_REG_OFF   = 0x3


# ═══════════════════════════════════════════════════════════════════════
# Namespace → Opcode mapping
# ═══════════════════════════════════════════════════════════════════════

NAMESPACE_MAP = {
    "Storage": {
        "Load":  OP_LOAD,
        "Save":  OP_SAVE,
        "Pipe":  OP_PIPE,
    },
    "Thread": {
        "Skip":  OP_NOOP,
        "Wait":  OP_WAIT,
        "Raise": OP_RAISE,
    },
    "Math": {
        "Add":   OP_ADD,
        "Sub":   OP_SUB,
        "Mul":   OP_MUL,
        "Div":   OP_DIV,
        "Inc":   OP_INC,
    },
    "Flow": {
        "Jump":   OP_JUMP,
        "Branch": OP_BRANCH,
        "Call":   OP_CALL,
        "Return": OP_RETURN,
    },
    "Dsp": {
        "MatMul":   (OP_DSP, DSP_MATMUL),
        "Softmax":  (OP_DSP, DSP_SOFTMAX),
        "Dot":      (OP_DSP, DSP_DOT),
        "Scale":    (OP_DSP, DSP_SCALE),
        "Relu":     (OP_DSP, DSP_RELU),
        "Norm":     (OP_DSP, DSP_NORM),
        "TopK":     (OP_DSP, DSP_TOPK),
        "Gelu":     (OP_DSP, DSP_GELU),
        "Transpose":(OP_DSP, DSP_TRANSPOSE),
        "VAdd":     (OP_DSP, DSP_VADD),
        "Embed":    (OP_DSP, DSP_EMBED),
        "Quant":    (OP_DSP, DSP_QUANT),
        "Dequant":  (OP_DSP, DSP_DEQUANT),
        "Mask":     (OP_DSP, DSP_MASK),
        "Concat":   (OP_DSP, DSP_CONCAT),
        "Split":    (OP_DSP, DSP_SPLIT),
    },
    "Net": {
        "Status":  OP_NOOP,  # Encoded as NOOP + metadata (HTTP framer reads imm16)
        "Header":  OP_NOOP,  # Hardware HTTP framer handles these
        "Type":    OP_NOOP,  # Content-Type shorthand
        "Body":    OP_NOOP,  # End headers, start body
        "Close":   OP_NOOP,  # Close connection
    },
}

# Net.* uses NOOP opcode but with special imm16 encoding that the
# hardware HTTP framer intercepts. This keeps the opcode space at 16.
NET_STATUS_BASE  = 0x8000  # imm16 bit 15 set = HTTP control
NET_HEADER_BASE  = 0x9000
NET_TYPE_BASE    = 0xA000
NET_BODY_MARKER  = 0xB000
NET_CLOSE_MARKER = 0xC000

# Common HTTP status codes
HTTP_STATUS = {
    200: 0x8000 | 200,
    201: 0x8000 | 201,
    204: 0x8000 | 204,
    301: 0x8000 | 301,
    302: 0x8000 | 302,
    400: 0x8000 | 400,
    401: 0x8000 | 401,
    403: 0x8000 | 403,
    404: 0x8000 | 404,
    500: 0x8000 | 500,
}

# Content-Type shortcuts
CONTENT_TYPES = {
    "text/html":        0xA000,
    "text/plain":       0xA001,
    "application/json": 0xA002,
    "text/css":         0xA003,
    "text/javascript":  0xA004,
    "image/png":        0xA005,
    "image/jpeg":       0xA006,
    "application/octet-stream": 0xA007,
}

# Branch condition name mapping
CONDITION_MAP = {
    "EQ": COND_EQ, "NE": COND_NE, "LT": COND_LT, "GT": COND_GT,
    "LE": COND_LE, "GE": COND_GE, "Z": COND_Z,   "NZ": COND_NZ,
    "EOF": COND_EOF, "ERR": COND_ERR,
}


# ═══════════════════════════════════════════════════════════════════════
# Card address encoding
# ═══════════════════════════════════════════════════════════════════════

def encode_card_addr(tenant, pack, card):
    """Encode tenant/pack/card into 16-bit address.

    Layout: [15:11]=tenant(32) [10:5]=pack(64) [4:0]=card(32)
    """
    assert 0 <= tenant <= 31, f"tenant {tenant} out of range (0-31)"
    assert 0 <= pack <= 63, f"pack {pack} out of range (0-63)"
    assert 0 <= card <= 31, f"card {card} out of range (0-31)"
    return (tenant << 11) | (pack << 5) | card


def decode_card_addr(addr16):
    """Decode 16-bit address into tenant/pack/card."""
    tenant = (addr16 >> 11) & 0x1F
    pack = (addr16 >> 5) & 0x3F
    card = addr16 & 0x1F
    return tenant, pack, card


# ═══════════════════════════════════════════════════════════════════════
# Compiler: source text → bytecode
# ═══════════════════════════════════════════════════════════════════════

def parse_register(s):
    """Parse 'R0'-'R15' into register number."""
    s = s.strip()
    if s.upper().startswith("R"):
        n = int(s[1:])
        assert 0 <= n <= 15, f"Register {s} out of range"
        return n
    raise ValueError(f"Expected register (R0-R15), got '{s}'")


def parse_arg(s, labels=None):
    """Parse an argument: register, integer, label, or string."""
    s = s.strip()
    if s.upper().startswith("R"):
        return ("reg", int(s[1:]))
    if s.startswith(":"):
        return ("label", s[1:])
    if s.startswith('"') or s.startswith("'"):
        return ("str", s[1:-1])
    if s.startswith("Stream."):
        return ("stream", s.split(".")[1])
    if s in CONDITION_MAP:
        return ("cond", CONDITION_MAP[s])
    try:
        return ("imm", int(s))
    except ValueError:
        pass
    # Check content-type strings
    if s in CONTENT_TYPES:
        return ("ctype", CONTENT_TYPES[s])
    return ("sym", s)


def encode_instruction(opcode, rd=0, rs1=0, rs2=0, imm16=0):
    """Encode a 32-bit PicoScript instruction."""
    return (opcode << 28) | (rd << 24) | (rs1 << 20) | (rs2 << 16) | (imm16 & 0xFFFF)


class Compiler:
    """Single-pass compiler: PicoScript source → bytecode.

    Two passes:
      1. Collect labels and their instruction indices
      2. Emit bytecode with resolved label offsets
    """

    def __init__(self):
        self.instructions = []
        self.labels = {}
        self.source_lines = []

    def compile(self, source):
        """Compile source text to list of 32-bit instruction words."""
        lines = source.strip().split("\n")
        # Pass 1: collect labels, strip comments/blanks
        clean_lines = []
        pc = 0
        for line in lines:
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            if line.startswith(":"):
                label = line[1:].rstrip(";").strip()
                self.labels[label] = pc
                continue
            clean_lines.append(line)
            pc += 1

        # Pass 2: compile each statement
        self.instructions = []
        for i, line in enumerate(clean_lines):
            self.source_lines.append(line)
            word = self._compile_statement(line, i)
            self.instructions.append(word)

        return self.instructions

    def _compile_statement(self, line, pc):
        """Compile a single Namespace.Method(args); statement."""
        # Strip trailing semicolon
        line = line.rstrip(";").strip()

        # Parse Namespace.Method(args)
        dot_pos = line.index(".")
        paren_pos = line.index("(")
        namespace = line[:dot_pos]
        method = line[dot_pos+1:paren_pos]
        args_str = line[paren_pos+1:line.rindex(")")]
        args = [a.strip() for a in args_str.split(",") if a.strip()] if args_str.strip() else []

        # Look up opcode
        ns_map = NAMESPACE_MAP.get(namespace)
        if ns_map is None:
            raise SyntaxError(f"Unknown namespace '{namespace}' at line {pc}: {line}")
        op_entry = ns_map.get(method)
        if op_entry is None:
            raise SyntaxError(f"Unknown method '{namespace}.{method}' at line {pc}: {line}")

        # Handle DSP (tuple: opcode + sub-op)
        if isinstance(op_entry, tuple):
            opcode, sub_op = op_entry
            return self._compile_dsp(sub_op, args, pc)

        opcode = op_entry

        # Dispatch by namespace
        if namespace == "Storage":
            return self._compile_storage(opcode, method, args, pc)
        elif namespace == "Thread":
            return self._compile_thread(opcode, method, args, pc)
        elif namespace == "Math":
            return self._compile_math(opcode, method, args, pc)
        elif namespace == "Flow":
            return self._compile_flow(opcode, method, args, pc)
        elif namespace == "Net":
            return self._compile_net(method, args, pc)
        else:
            raise SyntaxError(f"Unhandled namespace '{namespace}' at line {pc}")

    def _compile_storage(self, opcode, method, args, pc):
        """Storage.Load/Save/Pipe(tenant, pack, card, reg/stream)"""
        if len(args) != 4:
            raise SyntaxError(f"Storage.{method} requires 4 args (tenant, pack, card, target)")
        tenant = int(args[0])
        pack = int(args[1])
        card = int(args[2])
        addr16 = encode_card_addr(tenant, pack, card)
        target = parse_arg(args[3])
        rd = target[1] if target[0] == "reg" else 0
        return encode_instruction(opcode, rd=rd, imm16=addr16)

    def _compile_thread(self, opcode, method, args, pc):
        """Thread.Skip() / Thread.Wait() / Thread.Raise(channel)"""
        if method == "Raise" and args:
            channel = int(args[0])
            return encode_instruction(opcode, imm16=channel)
        return encode_instruction(opcode)

    def _compile_math(self, opcode, method, args, pc):
        """Math.Add(dest, src, value) / Math.Inc(reg)"""
        if method == "Inc":
            rd = parse_register(args[0])
            return encode_instruction(opcode, rd=rd)
        # ADD/SUB/MUL/DIV: dest, src, imm_or_reg
        rd = parse_register(args[0])
        rs1 = parse_register(args[1])
        third = parse_arg(args[2])
        if third[0] == "imm":
            return encode_instruction(opcode, rd=rd, rs1=rs1, imm16=third[1])
        elif third[0] == "reg":
            return encode_instruction(opcode, rd=rd, rs1=rs1, rs2=ADDR_REGISTER, imm16=third[1])
        raise SyntaxError(f"Math.{method}: third arg must be immediate or register")

    def _compile_flow(self, opcode, method, args, pc):
        """Flow.Jump(:label) / Flow.Branch(cond, Ra, Rb, :label) / Flow.Call(:label) / Flow.Return()"""
        if method == "Return":
            return encode_instruction(OP_RETURN)
        if method == "Jump":
            label = args[0].lstrip(":")
            target_pc = self.labels.get(label, 0)
            return encode_instruction(OP_JUMP, imm16=target_pc)
        if method == "Call":
            label = args[0].lstrip(":")
            target_pc = self.labels.get(label, 0)
            return encode_instruction(OP_CALL, imm16=target_pc)
        if method == "Branch":
            cond = CONDITION_MAP[args[0]]
            rd = parse_register(args[1])
            rs1 = parse_register(args[2])
            label = args[3].lstrip(":")
            target_pc = self.labels.get(label, 0)
            offset = target_pc - pc
            imm16 = offset & 0xFFFF
            return encode_instruction(OP_BRANCH, rd=rd, rs1=rs1, rs2=cond, imm16=imm16)
        raise SyntaxError(f"Unknown Flow method: {method}")

    def _compile_net(self, method, args, pc):
        """Net.Status(200) / Net.Type("text/html") / Net.Body() / Net.Close()"""
        if method == "Status":
            code = int(args[0])
            return encode_instruction(OP_NOOP, imm16=NET_STATUS_BASE | code)
        elif method == "Type":
            ct = args[0].strip('"').strip("'")
            imm = CONTENT_TYPES.get(ct, 0xA000)
            return encode_instruction(OP_NOOP, imm16=imm)
        elif method == "Header":
            # Custom header: encode as index
            return encode_instruction(OP_NOOP, imm16=NET_HEADER_BASE)
        elif method == "Body":
            return encode_instruction(OP_NOOP, imm16=NET_BODY_MARKER)
        elif method == "Close":
            return encode_instruction(OP_NOOP, imm16=NET_CLOSE_MARKER)
        raise SyntaxError(f"Unknown Net method: {method}")

    def _compile_dsp(self, sub_op, args, pc):
        """Dsp.MatMul(dest, src) / Dsp.Dot(dest, srcA, srcB) etc."""
        rd = parse_register(args[0]) if args else 0
        rs1 = parse_register(args[1]) if len(args) > 1 else 0
        imm16 = 0
        if len(args) > 2:
            third = parse_arg(args[2])
            if third[0] == "reg":
                imm16 = third[1]
            elif third[0] == "imm":
                imm16 = third[1]
        return encode_instruction(OP_DSP, rd=rd, rs1=rs1, rs2=sub_op, imm16=imm16)


# ═══════════════════════════════════════════════════════════════════════
# Disassembler: bytecode → source
# ═══════════════════════════════════════════════════════════════════════

OPCODE_TO_NS = {
    OP_NOOP: ("Thread", "Skip"), OP_LOAD: ("Storage", "Load"),
    OP_SAVE: ("Storage", "Save"), OP_PIPE: ("Storage", "Pipe"),
    OP_ADD: ("Math", "Add"), OP_SUB: ("Math", "Sub"),
    OP_MUL: ("Math", "Mul"), OP_DIV: ("Math", "Div"),
    OP_INC: ("Math", "Inc"), OP_JUMP: ("Flow", "Jump"),
    OP_BRANCH: ("Flow", "Branch"), OP_CALL: ("Flow", "Call"),
    OP_RETURN: ("Flow", "Return"), OP_WAIT: ("Thread", "Wait"),
    OP_RAISE: ("Thread", "Raise"), OP_DSP: ("Dsp", ""),
}

DSP_METHOD_NAMES = {
    DSP_MATMUL: "MatMul", DSP_SOFTMAX: "Softmax", DSP_DOT: "Dot",
    DSP_SCALE: "Scale", DSP_RELU: "Relu", DSP_NORM: "Norm",
    DSP_TOPK: "TopK", DSP_GELU: "Gelu", DSP_TRANSPOSE: "Transpose",
    DSP_VADD: "VAdd", DSP_EMBED: "Embed", DSP_QUANT: "Quant",
    DSP_DEQUANT: "Dequant", DSP_MASK: "Mask", DSP_CONCAT: "Concat",
    DSP_SPLIT: "Split",
}

COND_NAMES = {v: k for k, v in CONDITION_MAP.items()}


def disassemble(words):
    """Disassemble bytecode back to PicoScript source."""
    lines = []
    for i, word in enumerate(words):
        opcode = (word >> 28) & 0xF
        rd = (word >> 24) & 0xF
        rs1 = (word >> 20) & 0xF
        rs2 = (word >> 16) & 0xF
        imm16 = word & 0xFFFF

        # Check for Net.* (NOOP with high bit set in imm16)
        if opcode == OP_NOOP and imm16 & 0x8000:
            if imm16 & 0xF000 == 0x8000:
                lines.append(f"    Net.Status({imm16 & 0x1FF});")
            elif imm16 & 0xF000 == 0xA000:
                ct_name = next((k for k, v in CONTENT_TYPES.items() if v == imm16), "?")
                lines.append(f'    Net.Type("{ct_name}");')
            elif imm16 == NET_BODY_MARKER:
                lines.append("    Net.Body();")
            elif imm16 == NET_CLOSE_MARKER:
                lines.append("    Net.Close();")
            else:
                lines.append(f"    Net.Header({imm16:#06x});")
            continue

        ns, method = OPCODE_TO_NS.get(opcode, ("?", "?"))

        if opcode == OP_DSP:
            method = DSP_METHOD_NAMES.get(rs2, f"Sub{rs2}")
            if imm16:
                lines.append(f"    Dsp.{method}(R{rd}, R{rs1}, {imm16});")
            else:
                lines.append(f"    Dsp.{method}(R{rd}, R{rs1});")
        elif opcode in (OP_LOAD, OP_SAVE, OP_PIPE):
            t, p, c = decode_card_addr(imm16)
            lines.append(f"    {ns}.{method}({t}, {p}, {c}, R{rd});")
        elif opcode in (OP_ADD, OP_SUB, OP_MUL, OP_DIV):
            if rs2 == ADDR_REGISTER:
                lines.append(f"    {ns}.{method}(R{rd}, R{rs1}, R{imm16});")
            else:
                lines.append(f"    {ns}.{method}(R{rd}, R{rs1}, {imm16});")
        elif opcode == OP_INC:
            lines.append(f"    Math.Inc(R{rd});")
        elif opcode == OP_JUMP:
            lines.append(f"    Flow.Jump(:{imm16});")
        elif opcode == OP_CALL:
            lines.append(f"    Flow.Call(:{imm16});")
        elif opcode == OP_BRANCH:
            cond = COND_NAMES.get(rs2, f"?{rs2}")
            offset = imm16 if imm16 < 0x8000 else imm16 - 0x10000
            target = i + offset
            lines.append(f"    Flow.Branch({cond}, R{rd}, R{rs1}, :{target});")
        elif opcode == OP_RETURN:
            lines.append("    Flow.Return();")
        elif opcode == OP_NOOP:
            lines.append("    Thread.Skip();")
        elif opcode == OP_WAIT:
            lines.append("    Thread.Wait();")
        elif opcode == OP_RAISE:
            lines.append(f"    Thread.Raise({imm16});")
        else:
            lines.append(f"    // ??? opcode={opcode:#x} rd={rd} rs1={rs1} rs2={rs2} imm={imm16}")

    return "\n".join(lines)


# ═══════════════════════════════════════════════════════════════════════
# Multi-syntax decompilers (bytecode → different source views)
# ═══════════════════════════════════════════════════════════════════════

# All syntaxes are CRLF terminated. The card stores only bytecode.
# These are CLIENT-SIDE view transforms — zero cost on the device.

def _decode_word(word):
    """Common decode helper."""
    return {
        "opcode": (word >> 28) & 0xF,
        "rd": (word >> 24) & 0xF,
        "rs1": (word >> 20) & 0xF,
        "rs2": (word >> 16) & 0xF,
        "imm16": word & 0xFFFF,
    }


def decompile_csharp(words):
    """Decompile to C# style: Namespace.Method(args);\\r\\n"""
    # This is the same as disassemble() but with CRLF
    return disassemble(words).replace("\n", "\r\n")


def decompile_basic(words):
    """Decompile to C64 BASIC style: BLOCK TEXT, SPACED, COMMA SEPARATED\\r\\n

    Example:
        10 NET STATUS, 200
        20 NET TYPE, TEXT/HTML
        30 NET BODY
        40 STORAGE PIPE, 0, 1, 0, R0
        50 FLOW RETURN
    """
    # BASIC-style DSP names
    DSP_BASIC = {
        DSP_MATMUL: "MATMUL", DSP_SOFTMAX: "SOFTMAX", DSP_DOT: "DOT",
        DSP_SCALE: "SCALE", DSP_RELU: "RELU", DSP_NORM: "NORM",
        DSP_TOPK: "TOPK", DSP_GELU: "GELU", DSP_TRANSPOSE: "TRANSPOSE",
        DSP_VADD: "VADD", DSP_EMBED: "EMBED", DSP_QUANT: "QUANT",
        DSP_DEQUANT: "DEQUANT", DSP_MASK: "MASK", DSP_CONCAT: "CONCAT",
        DSP_SPLIT: "SPLIT",
    }
    CT_BASIC = {v: k.upper().replace("/", "/") for k, v in CONTENT_TYPES.items()}

    lines = []
    for i, word in enumerate(words):
        d = _decode_word(word)
        opcode = d["opcode"]
        rd, rs1, rs2, imm16 = d["rd"], d["rs1"], d["rs2"], d["imm16"]
        lineno = (i + 1) * 10

        # Net.* (NOOP with high bit)
        if opcode == OP_NOOP and imm16 & 0x8000:
            if imm16 & 0xF000 == 0x8000:
                lines.append(f"{lineno} NET STATUS, {imm16 & 0x1FF}")
            elif imm16 & 0xF000 == 0xA000:
                ct = CT_BASIC.get(imm16, f"TYPE/{imm16 & 0xFFF}")
                lines.append(f"{lineno} NET TYPE, {ct}")
            elif imm16 == NET_BODY_MARKER:
                lines.append(f"{lineno} NET BODY")
            elif imm16 == NET_CLOSE_MARKER:
                lines.append(f"{lineno} NET CLOSE")
            else:
                lines.append(f"{lineno} NET HEADER, {imm16:#06X}")
            continue

        if opcode == OP_NOOP:
            lines.append(f"{lineno} THREAD SKIP")
        elif opcode == OP_WAIT:
            lines.append(f"{lineno} THREAD WAIT")
        elif opcode == OP_RAISE:
            lines.append(f"{lineno} THREAD RAISE, {imm16}")
        elif opcode in (OP_LOAD, OP_SAVE, OP_PIPE):
            t, p, c = decode_card_addr(imm16)
            cmd = {OP_LOAD: "LOAD", OP_SAVE: "SAVE", OP_PIPE: "PIPE"}[opcode]
            lines.append(f"{lineno} STORAGE {cmd}, {t}, {p}, {c}, R{rd}")
        elif opcode in (OP_ADD, OP_SUB, OP_MUL, OP_DIV):
            cmd = {OP_ADD: "ADD", OP_SUB: "SUB", OP_MUL: "MUL", OP_DIV: "DIV"}[opcode]
            if rs2 == ADDR_REGISTER:
                lines.append(f"{lineno} MATH {cmd}, R{rd}, R{rs1}, R{imm16}")
            else:
                lines.append(f"{lineno} MATH {cmd}, R{rd}, R{rs1}, {imm16}")
        elif opcode == OP_INC:
            lines.append(f"{lineno} MATH INC, R{rd}")
        elif opcode == OP_JUMP:
            lines.append(f"{lineno} FLOW JUMP, {imm16 * 10 + 10}")
        elif opcode == OP_CALL:
            lines.append(f"{lineno} FLOW CALL, {imm16 * 10 + 10}")
        elif opcode == OP_BRANCH:
            cond = COND_NAMES.get(rs2, f"?{rs2}")
            offset = imm16 if imm16 < 0x8000 else imm16 - 0x10000
            target = (i + offset) * 10 + 10
            lines.append(f"{lineno} FLOW BRANCH, {cond}, R{rd}, R{rs1}, {target}")
        elif opcode == OP_RETURN:
            lines.append(f"{lineno} FLOW RETURN")
        elif opcode == OP_DSP:
            method = DSP_BASIC.get(rs2, f"OP{rs2}")
            if imm16:
                lines.append(f"{lineno} DSP {method}, R{rd}, R{rs1}, {imm16}")
            else:
                lines.append(f"{lineno} DSP {method}, R{rd}, R{rs1}")
        else:
            lines.append(f"{lineno} REM UNKNOWN {word:08X}")

    return "\r\n".join(lines) + "\r\n"


def decompile_python(words):
    """Decompile to Python style: namespace.method(args)\\r\\n

    Example:
        net.status(200)
        net.type("text/html")
        net.body()
        storage.pipe(0, 1, 0, r0)
        flow.ret()
    """
    DSP_PY = {
        DSP_MATMUL: "matmul", DSP_SOFTMAX: "softmax", DSP_DOT: "dot",
        DSP_SCALE: "scale", DSP_RELU: "relu", DSP_NORM: "norm",
        DSP_TOPK: "topk", DSP_GELU: "gelu", DSP_TRANSPOSE: "transpose",
        DSP_VADD: "vadd", DSP_EMBED: "embed", DSP_QUANT: "quant",
        DSP_DEQUANT: "dequant", DSP_MASK: "mask", DSP_CONCAT: "concat",
        DSP_SPLIT: "split",
    }
    CT_REV = {v: k for k, v in CONTENT_TYPES.items()}

    lines = []
    for i, word in enumerate(words):
        d = _decode_word(word)
        opcode = d["opcode"]
        rd, rs1, rs2, imm16 = d["rd"], d["rs1"], d["rs2"], d["imm16"]

        if opcode == OP_NOOP and imm16 & 0x8000:
            if imm16 & 0xF000 == 0x8000:
                lines.append(f"net.status({imm16 & 0x1FF})")
            elif imm16 & 0xF000 == 0xA000:
                ct = CT_REV.get(imm16, "application/octet-stream")
                lines.append(f'net.type("{ct}")')
            elif imm16 == NET_BODY_MARKER:
                lines.append("net.body()")
            elif imm16 == NET_CLOSE_MARKER:
                lines.append("net.close()")
            else:
                lines.append(f"net.header({imm16:#06x})")
            continue

        if opcode == OP_NOOP:
            lines.append("thread.skip()")
        elif opcode == OP_WAIT:
            lines.append("thread.wait()")
        elif opcode == OP_RAISE:
            lines.append(f"thread.raise_irq({imm16})")
        elif opcode in (OP_LOAD, OP_SAVE, OP_PIPE):
            t, p, c = decode_card_addr(imm16)
            cmd = {OP_LOAD: "load", OP_SAVE: "save", OP_PIPE: "pipe"}[opcode]
            lines.append(f"storage.{cmd}({t}, {p}, {c}, r{rd})")
        elif opcode in (OP_ADD, OP_SUB, OP_MUL, OP_DIV):
            cmd = {OP_ADD: "add", OP_SUB: "sub", OP_MUL: "mul", OP_DIV: "div"}[opcode]
            if rs2 == ADDR_REGISTER:
                lines.append(f"math.{cmd}(r{rd}, r{rs1}, r{imm16})")
            else:
                lines.append(f"math.{cmd}(r{rd}, r{rs1}, {imm16})")
        elif opcode == OP_INC:
            lines.append(f"math.inc(r{rd})")
        elif opcode == OP_JUMP:
            lines.append(f"flow.jump(:{imm16})")
        elif opcode == OP_CALL:
            lines.append(f"flow.call(:{imm16})")
        elif opcode == OP_BRANCH:
            cond = COND_NAMES.get(rs2, f"?{rs2}").lower()
            offset = imm16 if imm16 < 0x8000 else imm16 - 0x10000
            target = i + offset
            lines.append(f"flow.branch({cond}, r{rd}, r{rs1}, :{target})")
        elif opcode == OP_RETURN:
            lines.append("flow.ret()")
        elif opcode == OP_DSP:
            method = DSP_PY.get(rs2, f"op{rs2}")
            if imm16:
                lines.append(f"dsp.{method}(r{rd}, r{rs1}, {imm16})")
            else:
                lines.append(f"dsp.{method}(r{rd}, r{rs1})")
        else:
            lines.append(f"# unknown {word:08X}")

    return "\r\n".join(lines) + "\r\n"


def decompile_hex(words):
    """Raw hex dump: one 32-bit word per line.\\r\\n"""
    return "\r\n".join(f"{w:08X}" for w in words) + "\r\n"


# Available syntax modes
SYNTAXES = {
    "csharp": {"name": "C#",     "ext": ".pico",  "decompile": decompile_csharp},
    "basic":  {"name": "BASIC",  "ext": ".bas",   "decompile": decompile_basic},
    "python": {"name": "Python", "ext": ".py",    "decompile": decompile_python},
    "hex":    {"name": "Hex",    "ext": ".hex",   "decompile": decompile_hex},
}


# ═══════════════════════════════════════════════════════════════════════
# Example programs
# ═══════════════════════════════════════════════════════════════════════

EXAMPLE_HELLO = """\
// hello.pico -- Serve a static HTML page
Net.Status(200);
Net.Type("text/html");
Net.Body();
Storage.Pipe(0, 1, 0, R0);
Flow.Return();
"""

EXAMPLE_API = """\
// api_user.pico -- JSON API: GET /api/users/{id}
// R15 = connection context (card address from URL)
Net.Status(200);
Net.Type("application/json");
Net.Body();
Storage.Load(0, 2, 0, R0);
Flow.Branch(Z, R0, R0, :notfound);
Storage.Pipe(0, 2, 0, R0);
Flow.Return();
:notfound
Net.Status(404);
Net.Type("application/json");
Net.Body();
Storage.Pipe(0, 0, 31, R0);
Flow.Return();
"""

EXAMPLE_FILTER = """\
// filter.pico -- Loop over cards, emit matching ones
Math.Add(R0, R0, 100);
Math.Add(R1, R1, 110);
Math.Add(R2, R2, 50);
:loop
Storage.Load(0, 3, 0, R3);
Flow.Branch(LE, R3, R2, :skip);
Storage.Pipe(0, 3, 0, R0);
:skip
Math.Inc(R0);
Flow.Branch(LT, R0, R1, :loop);
Flow.Return();
"""

EXAMPLE_AI = """\
// inference.pico -- Dot product + softmax (vector search)
Storage.Load(0, 10, 0, R0);
Storage.Load(0, 10, 1, R1);
Dsp.Dot(R2, R0, R1);
Dsp.Softmax(R3, R2);
Dsp.TopK(R4, R3, 10);
Net.Status(200);
Net.Type("application/json");
Net.Body();
Storage.Save(0, 0, 30, R4);
Storage.Pipe(0, 0, 30, R4);
Flow.Return();
"""

EXAMPLE_EVENT = """\
// event.pico -- Wait for interrupt, then process
:wait_loop
Thread.Wait();
Storage.Load(0, 5, 0, R0);
Net.Status(200);
Net.Type("text/plain");
Net.Body();
Storage.Pipe(0, 5, 0, R0);
Flow.Jump(:wait_loop);
"""


# ═══════════════════════════════════════════════════════════════════════
# Main: demo compile + disassemble round-trip
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("PicoScript Language v1.0 -- Multi-Syntax Bytecode Views")
    print("=" * 65)
    print()
    print("Cards store ONLY bytecode (4 bytes per instruction).")
    print("The 'language' is a client-side view. Compile on save, decompile on load.")
    print("Same card, different view. Zero cost on device.")
    print()
    print("Available syntaxes:")
    for key, info in SYNTAXES.items():
        print(f"  {info['name']:8s} ({info['ext']})")
    print()

    # Compile the hello world example once
    print("─" * 65)
    print("EXAMPLE: Hello World — same bytecode, 4 different views")
    print("─" * 65)
    print()

    compiler = Compiler()
    bytecode = compiler.compile(EXAMPLE_HELLO)

    print(f"Card bytecode ({len(bytecode)} words, {len(bytecode)*4} bytes):")
    for i, word in enumerate(bytecode):
        print(f"  [{i:2d}] {word:08X}")
    print()

    print("┌─── View as C# (.pico) ────────────────────────────────────────┐")
    for line in decompile_csharp(bytecode).replace("\r\n", "\n").strip().split("\n"):
        print(f"│ {line:62s}│")
    print("└────────────────────────────────────────────────────────────────┘")
    print()

    print("┌─── View as BASIC (.bas) ───────────────────────────────────────┐")
    for line in decompile_basic(bytecode).replace("\r\n", "\n").strip().split("\n"):
        print(f"│ {line:62s}│")
    print("└────────────────────────────────────────────────────────────────┘")
    print()

    print("┌─── View as Python (.py) ───────────────────────────────────────┐")
    for line in decompile_python(bytecode).replace("\r\n", "\n").strip().split("\n"):
        print(f"│ {line:62s}│")
    print("└────────────────────────────────────────────────────────────────┘")
    print()

    print("┌─── View as Hex (.hex) ─────────────────────────────────────────┐")
    for line in decompile_hex(bytecode).replace("\r\n", "\n").strip().split("\n"):
        print(f"│ {line:62s}│")
    print("└────────────────────────────────────────────────────────────────┘")
    print()

    # Show a more complex example in BASIC
    print("─" * 65)
    print("EXAMPLE: Filter loop — BASIC style")
    print("─" * 65)
    print()
    compiler2 = Compiler()
    bytecode2 = compiler2.compile(EXAMPLE_FILTER)
    print(decompile_basic(bytecode2).replace("\r\n", "\n"))

    # Show AI example in all styles
    print("─" * 65)
    print("EXAMPLE: AI inference — all styles")
    print("─" * 65)
    print()
    compiler3 = Compiler()
    bytecode3 = compiler3.compile(EXAMPLE_AI)
    print("  C#:")
    print(decompile_csharp(bytecode3).replace("\r\n", "\n"))
    print("  BASIC:")
    print(decompile_basic(bytecode3).replace("\r\n", "\n"))
    print("  Python:")
    print(decompile_python(bytecode3).replace("\r\n", "\n"))

    # Architecture summary
    print("=" * 65)
    print("Architecture:")
    print()
    print("  ┌──────────────┐       ┌──────────────┐")
    print("  │ Browser/CLI  │       │   PicoWAL    │")
    print("  │              │       │   Device     │")
    print("  │ Edit in C#   │       │              │")
    print("  │ Edit in BASIC│  TCP  │  ┌────────┐  │")
    print("  │ Edit in Py   │◄─────►│  │BYTECODE│  │")
    print("  │              │       │  │ 4B/inst │  │")
    print("  │ Compile ─────┼──────►│  └────────┘  │")
    print("  │ Decompile ◄──┼───────┤   SD card    │")
    print("  └──────────────┘       └──────────────┘")
    print()
    print("  • Card stores bytecode only (never text)")
    print("  • Client picks display syntax (preference)")
    print("  • Compile: text → bytecode (trivial, 1:1)")
    print("  • Decompile: bytecode → text (trivial, 1:1)")
    print("  • Could even decompile in a DIFFERENT language than you wrote in")
    print("  • Write in Python, colleague reads it in C#. Same card.")
    print("  • All CRLF terminated for universal compatibility")
    print("=" * 65)
