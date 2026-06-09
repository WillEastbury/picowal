# PicoScript Hardware Specification

This document is the hardware-facing PicoScript contract. It defines the bytecode, execution model, and RTL-visible behaviours that must remain stable while the source language and editor UX evolve independently.

The language compiler may add syntax, aliases, refactorings, or editor conveniences, but it must emit bytecode that conforms to this specification.

## Scope

Hardware owns:

- Instruction word layout and opcode meanings
- Register file shape and special registers
- Card address encoding
- Program counter, call stack, branch, wait, and interrupt semantics
- Execution-unit routing: scheduler, memory, ALU, branch, PIPE, HTTP, DSP
- Cycle-level and resource-budget constraints

Hardware does not own:

- Source syntax
- Formatting
- Editor display language
- Symbol names beyond the compiled bytecode contract
- Refactoring or autocomplete behaviour

## Instruction Word

PicoScript bytecode uses fixed-width 32-bit instructions:

```text
[31:28] opcode   4-bit core opcode
[27:24] Rd       destination register
[23:20] Rs1      source register 1
[19:16] Rs2      source register 2 / mode / condition / sub-op
[15:0]  imm16    immediate value / card address / branch target
```

Fixed-width encoding is the primary hardware constraint. It keeps decode combinatorial and predictable on small FPGAs.

## Registers

| Register | Role |
|----------|------|
| R0-R14 | General-purpose registers |
| R15 | Connection/context register, read-only to PicoScript programs |

The current RTL target stores 8 contexts x 16 registers x 32 bits.

## Card Address Encoding

Current compiler-facing address layout:

```text
[15:11] tenant  32 tenants
[10:5]  pack    64 packs per tenant
[4:0]   card    32 cards per pack
```

The address is produced by `encode_card_addr(tenant, pack, card)` in `picoscript_lang.py`.

## Core Opcodes

| Op | Mnemonic | Function | Primary unit |
|:--:|----------|----------|--------------|
| 0 | NOOP | No-op / HTTP control envelope | Scheduler / HTTP framer |
| 1 | LOAD | Load card to register | Memory controller |
| 2 | SAVE | Save register to card | Memory controller |
| 3 | PIPE | Stream card to TCP output | PIPE engine |
| 4 | ADD | Addition | ALU |
| 5 | SUB | Subtraction | ALU |
| 6 | MUL | Multiplication | ALU / soft MAC |
| 7 | DIV | Division | ALU / divider |
| 8 | INC | Increment | ALU |
| 9 | JUMP | Unconditional branch | Branch unit |
| A | BRANCH | Conditional branch | Branch unit |
| B | CALL | Push return address and jump | Branch unit / call stack |
| C | RETURN | Pop return address | Branch unit / call stack |
| D | WAIT | Suspend context | Scheduler |
| E | RAISE | Wake waiting context/channel | IRQ / scheduler |
| F | DSP | DSP sub-operation envelope | DSP / MAC engine |

The detailed opcode reference lives in `picoscript_opcodes.py`. That file should stay hardware-oriented: encodings, cycle estimates, and execution units.

## Addressing Modes

For LOAD/SAVE/PIPE and arithmetic variants, `Rs2` selects mode where applicable:

| Mode | Meaning |
|:----:|---------|
| 0 | Immediate address/value from `imm16` |
| 1 | Register indirect |
| 2 | Base plus offset |
| 3 | Register plus offset |

Register-indirect LOAD/SAVE is the key capability that makes PicoScript able to follow computed card addresses.

## Branch Conditions

For `BRANCH`, `Rs2` encodes the condition:

| Code | Condition |
|:----:|-----------|
| 0 | EQ |
| 1 | NE |
| 2 | LT |
| 3 | GT |
| 4 | LE |
| 5 | GE |
| 6 | Z |
| 7 | NZ |
| 8 | EOF |
| 9 | ERR |

The compiler is responsible for resolving labels to bytecode offsets or absolute targets.

## DSP Sub-operations

Opcode `0xF` is an envelope. `Rs2` selects the DSP sub-operation:

| Sub-op | Mnemonic |
|:------:|----------|
| 0 | MATMUL |
| 1 | SOFTMAX |
| 2 | DOT |
| 3 | SCALE |
| 4 | RELU |
| 5 | NORM |
| 6 | TOPK |
| 7 | GELU |
| 8 | TRANSPOSE |
| 9 | VADD |
| A | EMBED |
| B | QUANT |
| C | DEQUANT |
| D | MASK |
| E | CONCAT |
| F | SPLIT |

Language-level names may change, but the emitted sub-op values must not unless the hardware spec is revised.

## HTTP Control Encoding

`NOOP` with `imm16[15]` set is reserved for HTTP control metadata intercepted by the HTTP framer:

| Range / value | Meaning |
|---------------|---------|
| `0x8xxx` | HTTP status |
| `0x9xxx` | Header |
| `0xAxxx` | Content type |
| `0xB000` | Body marker |
| `0xC000` | Close marker |

This is a bytecode-level mechanism. Editors may present it as `Net.Status`, BASIC `NET STATUS`, Python `net.status`, or another view.

## Execution Model

Each active context has:

- Register file view
- Program counter
- Flags
- Call stack
- Scheduler state

`WAIT` removes a context from scheduling until a matching `RAISE` or hardware event wakes it. PIPE, storage, and fork/join completion may also wake contexts.

## RTL Mapping

The checked-in RTL currently contains partial PicoScript hardware:

- `picowal_hx_cu/picoscript_decode.v`
- `picowal_hx_cu/picoscript_alu.v`
- `picowal_hx_cu/picoscript_branch.v`
- `picowal_hx_cu/context_scheduler.v`
- `picowal_hx_cu/pipe_engine.v`

The top-level Makefile still references missing implementation modules, so this spec is ahead of a complete buildable RTL implementation.

## Change Control

Changing this file means changing the hardware contract. Any change here should be checked against:

- `picoscript.py`
- `picoscript_opcodes.py`
- `picoscript_lang.py`
- `picowal_hx_cu/picoscript_decode.v`
- `picowal_hx_cu/picowal_hx_top.v`

Language/editor-only changes should usually be made in `docs/picoscript-language-editor.md` and `picoscript_lang.py` without changing this document.
