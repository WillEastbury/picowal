# PicoScript Language and Editor Specification

This document is the client-facing PicoScript language and editor contract. It is intentionally separate from the hardware specification so the source language, display syntaxes, diagnostics, and editing experience can be tuned without changing the FPGA bytecode contract.

The canonical hardware bytecode contract is `docs/picoscript-hardware.md`.

## Scope

Language/editor owns:

- Source syntax and aliases
- Namespace and method naming
- Label syntax
- Formatting and CRLF/LF handling
- Decompiler views
- Diagnostics
- Autocomplete metadata
- Refactoring rules
- Editor round-trip guarantees

Language/editor does not own:

- Opcode numbers
- Bit layout
- Register width
- Hardware cycle counts
- RTL module boundaries

## Core Principle

Cards store bytecode, not source text.

Source files are views over bytecode. The editor may let one user write C#-style PicoScript, another view the same card as BASIC, and another view it as Python-style calls. Save compiles source to bytecode. Load decompiles bytecode to the selected view.

## Current Syntax Views

`picoscript_lang.py` currently accepts C-style namespace calls and BASIC-style input, and supports these decompiler views:

| Mode | Extension | Example |
|------|-----------|---------|--------|
| C style | `.pico` | `Storage.Load(0, 1, 42, R0);` | Input + output |
| BASIC style | `.bas` | `10 STORAGE LOAD, 0, 1, 42, R0` | Input + output |
| Python style | `.py` | `storage.load(0, 1, 42, r0)` | Output view only |
| Hex | `.hex` | `1040002A` | Output view only |

Future Python or Hex parsers should reuse the same parse-to-IR layer, but they are not part of the current checked-in compiler contract.

## C-style Source Syntax

The C-style syntax is:

```csharp
Namespace.Method(arg0, arg1, ...);
```

Example:

```csharp
Net.Status(200);
Net.Type("text/html");
Net.Body();
Storage.Load(0, 1, 42, R0);
Flow.Branch(Z, R0, R0, :notfound);
Storage.Pipe(0, 1, 42, Stream.Out);
Flow.Return();
```

Labels start with `:` and bind to instruction indices:

```csharp
:loop
Math.Inc(R0);
Flow.Branch(LT, R0, R1, :loop);
```

Comments currently use `//`.

## BASIC-style Source Syntax

BASIC-style input uses optional ascending line numbers, uppercase command groups, and comma-separated operands:

```basic
10 NET STATUS, 200
20 NET TYPE, TEXT/HTML
30 NET BODY
40 STORAGE LOAD, 0, 1, 42, R0
50 FLOW BRANCH, NZ, R0, R0, 40
60 FLOW RETURN
```

Numeric flow targets are BASIC line numbers. The compiler maps line numbers to instruction indices before emitting bytecode.

## Namespaces

Namespaces are language-facing names for hardware capabilities:

| Namespace | Purpose |
|-----------|---------|
| `Storage` | Card load, save, pipe |
| `Thread` | Skip, wait, raise |
| `Math` | Integer arithmetic |
| `Flow` | Jump, branch, call, return |
| `Dsp` | DSP envelope operations |
| `Net` | HTTP response metadata |

These names are editor-facing. The compiler maps them to opcode fields described in `docs/picoscript-hardware.md`.

## Current Compiler Surface

The checked-in compiler currently emits these source forms:

| Namespace | Methods |
|-----------|---------|
| `Storage` | `Load`, `Save`, `Pipe` |
| `Thread` | `Skip`, `Wait`, `Raise` |
| `Math` | `Add`, `Sub`, `Mul`, `Div`, `Inc` |
| `Flow` | `Jump`, `Branch`, `Call`, `Return` |
| `Dsp` | `MatMul`, `Softmax`, `Dot`, `Scale`, `Relu`, `Norm`, `TopK`, `Gelu`, `Transpose`, `VAdd`, `Embed`, `Quant`, `Dequant`, `Mask`, `Concat`, `Split` |
| `Net` | `Status`, `Header`, `Type`, `Body`, `Close` |

Planned hardware/editor primitives such as `Storage.Field`, `Storage.ForEach`, `Storage.Template`, `Thread.Fork`, `Thread.Join`, `Flow.For`, and `Flow.Switch` are documented as proposed extension points in `picoscript_opcodes.py`. They should not be exposed as accepted input until their encodings are stable in `docs/picoscript-hardware.md` and implemented consistently in the compiler, disassemblers, and runtime/RTL.

## Flow Target Resolution

C-style labels are instruction addresses, not source-line numbers. BASIC flow targets are BASIC line numbers that resolve to instruction addresses.

| Instruction | Label encoding |
|-------------|----------------|
| `Flow.Jump(:label)` | Absolute instruction index in `imm16` |
| `Flow.Call(:label)` | Absolute instruction index in `imm16` |
| `Flow.Branch(cond, Ra, Rb, :label)` | Relative signed offset from the branch instruction to the target instruction, stored in `imm16` two's-complement form |
| `FLOW JUMP, line` | Absolute instruction index for `line` in `imm16` |
| `FLOW CALL, line` | Absolute instruction index for `line` in `imm16` |
| `FLOW BRANCH, cond, Ra, Rb, line` | Relative signed offset from the branch instruction to `line` |

Unknown labels or BASIC line targets are compiler errors. They must not silently resolve to instruction 0.

## Editor Model

The editor should treat PicoScript as a structured bytecode view:

1. Parse source into statements and labels.
2. Resolve labels.
3. Emit 32-bit instruction words.
4. Store only bytecode in cards.
5. Decompile bytecode back into the selected display syntax.

Current C/BASIC bytecode round-trip invariants:

```text
C-style source -> bytecode -> BASIC source view -> bytecode
BASIC source -> bytecode -> C-style source view -> bytecode
C-style source -> bytecode -> C-style source view -> bytecode
BASIC source -> bytecode -> BASIC source view -> bytecode
```

The final bytecode should match unless the user edits semantics. Cross-view round-trips through Python/Hex require input parsers for those views and are planned editor work, not current compiler behaviour.

## Diagnostics

Diagnostics should be source-level and explain the hardware constraint when relevant:

| Error | Preferred diagnostic |
|-------|----------------------|
| Unknown namespace | `Unknown namespace 'X'. Expected Storage, Thread, Math, Flow, Dsp, or Net.` |
| Unknown method | `Unknown method 'Storage.X'.` |
| Bad register | `Expected register R0-R15.` |
| Card address out of range | `Card address fields must fit tenant=0-31, pack=0-63, card=0-31.` |
| Unknown label | `Unknown label ':name'. Define it with ':name' on its own line.` |
| Unknown BASIC line | `Unknown BASIC line N.` |
| Non-ascending BASIC line numbers | `BASIC line numbers must be unique and ascending.` |
| Immediate out of range | `Immediate must fit imm16.` |
| Unsupported input view | `Expected C-style Namespace.Method(...) or BASIC-style input.` |

Avoid hardware-centric errors like "bad Rs2" in the editor unless the user is in hex/assembly mode.

## Completion Metadata

The editor can derive completions from the language namespace table:

| Trigger | Suggestions |
|---------|-------------|
| start of statement | `Storage`, `Thread`, `Math`, `Flow`, `Dsp`, `Net` |
| `Storage.` | `Load`, `Save`, `Pipe` |
| `Thread.` | `Skip`, `Wait`, `Raise` |
| `Math.` | `Add`, `Sub`, `Mul`, `Div`, `Inc` |
| `Flow.` | `Jump`, `Branch`, `Call`, `Return` |
| `Dsp.` | `MatMul`, `Softmax`, `Dot`, `Scale`, `Relu`, `Norm`, `TopK`, `Gelu`, `Transpose`, `VAdd`, `Embed`, `Quant`, `Dequant`, `Mask`, `Concat`, `Split` |
| `Net.` | `Status`, `Header`, `Type`, `Body`, `Close` |

Register completions should offer `R0` through `R15`, with `R15` marked read-only/context.

The editor may show planned primitives in a separate "proposed hardware extension" group, but should not complete them as ordinary accepted compiler input until the compiler implements them.

## Formatting

Recommended C#-style formatting:

- One statement per line
- Labels on their own line
- Four-space indentation for statements under labels when displayed in examples
- Semicolon required in C# style
- Preserve comments where source text is available

Decompiler output currently uses CRLF so generated source views remain easy to consume across editors and terminals.

## Language Tuning Guidelines

Language changes are encouraged here as long as emitted bytecode remains stable. Good candidates:

- Friendlier aliases, such as `return;` mapping to `Flow.Return();`
- Safer high-level forms, such as `if R0 == R1 goto :done`
- Editor-only macros that expand deterministically to bytecode
- Better field/schema names that compile to numeric card fields
- Snippets for HTTP handlers, filters, scans, and template responders

Avoid adding features that hide unpredictable work from the hardware. PicoScript should remain a transparent view over finite bytecode.

## Parser Boundaries

The compiler should keep language parsing separate from bytecode emission:

```text
source text -> AST/statements -> resolved IR -> 32-bit words
```

The current implementation is a compact direct parser in `picoscript_lang.py`. As the editor grows, it should be split so autocomplete, formatting, diagnostics, and compilation reuse the same parse result.

## Files

Language/editor files:

- `picoscript_lang.py` - primary compiler, decompilers, examples
- `docs/picoscript-language-editor.md` - language/editor contract

Hardware contract files consumed by language tooling:

- `docs/picoscript-hardware.md`
- `picoscript.py`
- `picoscript_opcodes.py`
- `picowal_hx_cu/picoscript_decode.v`
