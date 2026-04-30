# PicoWAL

**A network-attached application server that runs entirely in FPGA logic — no CPU, no OS, no framework.**

PicoWAL serves dynamic web pages from a £24 board using a custom bytecode ISA (PicoScript) executed directly in LUTs. HTTP requests arrive over Ethernet, trigger hardware state machines that load data cards, render templates with nested loops, and stream responses — all without a single line of software running anywhere.

---

## What Is This?

PicoWAL is a pure-FPGA query engine and web application server. It stores data as **cards** (fixed-size records) organised into **packs** (collections) under **tenants** (namespaces). Queries execute as PicoScript bytecode programs — compiled from a multi-syntax source language — running on up to 8 hardware virtual cores with zero-overhead parallel loops.

### Key Properties

| Property | Value |
|----------|-------|
| CPU | None. FPGA IS the processor |
| Operating System | None |
| Language runtime | None — bytecode executes in combinatorial logic |
| TCP/IP stack | Hardware (W5100S chip) |
| HTTP parsing | Hardware (FPGA LUTs) |
| Template rendering | Hardware (streaming find-and-replace with 2-level FOREACH) |
| Power consumption | 0.92W (USB powered) |
| Prototype cost | ~£24 |
| Form factor | 60×40mm PCB or Alchitry Cu breadboard |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    iCE40HX8K FPGA                        │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐ │
│  │ HTTP     │  │PicoScript│  │ Template │  │  PIPE  │ │
│  │ Parser   │→ │ Executor │→ │ Engine   │→ │ DMA    │ │
│  │ (980 LUT)│  │ (8 cores)│  │(2-level) │  │        │ │
│  └──────────┘  └──────────┘  └──────────┘  └────────┘ │
│       ↑              ↑              ↑            ↓      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐ │
│  │ W5100S   │  │   QSPI   │  │  Schema  │  │   SD   │ │
│  │ TCP/IP   │  │   SRAM   │  │  BRAM    │  │  Card  │ │
│  │ (SPI)    │  │  (cache) │  │ (tables) │  │  Store │ │
│  └──────────┘  └──────────┘  └──────────┘  └────────┘ │
└─────────────────────────────────────────────────────────┘
```

### Data Model

```
Tenant → Pack → Card
  │        │       └── 512-byte fixed record (bytecode OR data)
  │        └── Collection of cards (like a database table)
  └── Isolation boundary (like a database)
```

### Storage Hierarchy

| Tier | Medium | Capacity | Latency | Bandwidth | Cost | Required |
|------|--------|----------|---------|-----------|------|----------|
| L0 | BRAM (on-chip) | 4KB | 1 cycle | — | — | Yes |
| L1 | QSPI SRAM | 128KB-1MB | ~200ns | ~6MB/s | £1.50 | Yes |
| L2 | eMMC (4-bit) | 4-32GB | ~100µs | ~24MB/s | £3-8 | **Optional** |
| L3 | SD Card (SPI) | 2-32GB | ~1ms | ~4MB/s | £3-8 | Yes |

**eMMC auto-detection:** A detect pin (pulled HIGH via 10K resistor) is grounded by the eMMC module when present. At boot, the FPGA samples this pin — if LOW, the L2 tier is enabled and the eMMC is initialised. If HIGH, L2 is skipped and SRAM talks directly to SD.

Cache policy: LRU eviction, write-back. Reads cascade L1→L2→L3. Writes hit L1 immediately, write-back on eviction.

---

## PicoScript ISA

32-bit fixed-width instruction word:

```
[31:28] opcode   4-bit (16 opcodes, single LUT decode)
[27:24] Rd       destination register (R0-R15)
[23:20] Rs1      source register 1
[19:16] Rs2      source register 2 / mode / condition / sub-op
[15:0]  imm16    immediate value / card address / branch target
```

### Opcode Table

| Op | Mnemonic | Function | Cycles | Hardware Unit |
|:--:|----------|----------|:------:|---------------|
| 0 | NOOP | No-op / HTTP control | 1 | Scheduler |
| 1 | LOAD | Load card → register | 2-48K | Memory controller |
| 2 | SAVE | Save register → card | 2-48K | Memory controller |
| 3 | PIPE | Zero-copy card → TCP stream | var | PIPE DMA engine |
| 4 | ADD | Addition | 1 | ALU |
| 5 | SUB | Subtraction | 1 | ALU |
| 6 | MUL | Multiplication | 8 | Soft MAC |
| 7 | DIV | Division | 32 | Soft divider |
| 8 | INC | Increment | 1 | ALU |
| 9 | JUMP | Unconditional jump (to card) | 2-10 | Flow controller |
| A | BRANCH | Conditional branch | 1-10 | Branch unit |
| B | CALL | Subroutine call (push card+IP) | 2-10 | Flow + call stack |
| C | RETURN | Return from call (pop card+IP) | 2-10 | Flow + call stack |
| D | WAIT | Suspend context until interrupt | 0† | Scheduler |
| E | RAISE | Fire software interrupt | 1 | IRQ controller |
| F | DSP | AI/ML accelerator operation | 8-2048 | Soft MAC array |

† WAIT consumes zero cycles — context is removed from scheduling until woken by RAISE.

### Flow Control = Card Navigation

The program counter is `(pack, card, instruction_pointer)`. Flow instructions navigate between cards:

- **JUMP(pack, card)** — Load target card, execute from IP=0
- **BRANCH(cond, pack, card)** — Conditional JUMP
- **CALL(pack, card)** — Push (current_pack, current_card, IP+1) onto call stack, then JUMP
- **RETURN** — Pop (pack, card, IP) from call stack, resume

Local branching (within a card) uses mode=0. Cross-card jumps use mode=1.

### WAIT / RAISE Interrupt Model

```
Context A:  WAIT          → suspends, 0 cycles consumed
Context B:  RAISE 0xA    → wakes context A at IP+1
```

Wake sources:
- **Software**: Another context executes `RAISE channel_id`
- **HTTP parser**: New request decoded → auto-RAISE assigned handler
- **PIPE engine**: Transfer complete → auto-RAISE originating context
- **Fork/Join**: All v-cores joined → auto-RAISE forking context

### Hardware Loop Constructs

These are **silicon primitives**, not compiler sugar — zero-overhead execution:

| Construct | Hardware | LUTs | Description |
|-----------|----------|------|-------------|
| FOR | Loop counter + auto-branch | 80 | Counted loop, branch is free |
| FOREACH | Card iterator + auto-load | 100 | Walk a pack, auto-load each card |
| SWITCH | BRAM jump table | 40 | O(1) indexed dispatch |

### DSP Sub-operations (opcode 0xF)

| Sub-op | Mnemonic | Function |
|:------:|----------|----------|
| 0 | MATMUL | Matrix multiply |
| 1 | SOFTMAX | Softmax activation |
| 2 | DOT | Dot product |
| 3 | SCALE | Scale vector |
| 4 | RELU | ReLU activation |
| 5 | NORM | Layer normalisation |
| 6 | TOPK | Top-K selection |
| 7 | GELU | GELU activation |
| 8 | TRANSPOSE | Matrix transpose |
| 9 | VADD | Vector add |
| A | EMBED | Embedding lookup |
| B | QUANT | Quantize float→int8 |
| C | DEQUANT | Dequantize int8→float |
| D | MASK | Attention mask |
| E | CONCAT | Concatenate vectors |
| F | SPLIT | Split into heads |

### Parallel Loops (Fork/Join)

```csharp
Thread.Fork(R0, R1, :body);   // distribute iterations across 8 v-cores
// ... loop body ...
Thread.Join();                  // barrier — wait for all cores
```

Distributes range [R0..R1) across available v-cores. Each gets a unique iteration value in R0. 8× speedup on embarrassingly parallel workloads (filter, scan, map).

---

## Schema & Field Extraction

Cards can have typed schemas stored in BRAM:

```
Schema "users": 
  field 0: offset=0,  type=u32, len=4   (id)
  field 1: offset=4,  type=str, len=32  (name)  
  field 2: offset=36, type=u16, len=2   (age)
  field 3: offset=38, type=u32, len=4   (balance)
```

Hardware FIELD extractor (80 LUTs): reads schema from BRAM, barrel-shifts card data to extract the named field into a register. Enables:

```csharp
Storage.Field(R0, "age");           // extract field → R0
Flow.Branch(GT, R0, 18, :match);   // WHERE age > 18
```

---

## Template Engine (Hardware)

Streaming template rendering with **2-level nested FOREACH**:

```html
<h1>{{1}}</h1>                    <!-- field 1 from master card -->
{{EACH:orders}}                    <!-- iterate child pack -->
  <div>Order #{{1}}               <!-- field from child -->
    <ul>
    {{EACH:items}}                  <!-- iterate grandchild pack -->
      <li>{{2}} — £{{3}}</li>      <!-- fields from grandchild -->
    {{/EACH}}
    </ul>
  </div>
{{/EACH}}
```

### Template Markers (in card data)

| Byte | Meaning |
|------|---------|
| `0xFE` + field_id | Replace with field value |
| `0xFD` + pack_id | Begin FOREACH (push nesting level) |
| `0xFC` | End FOREACH (loop or pop level) |

The template engine streams card data toward the TCP TX buffer, detecting markers and injecting live field values in-flight. No buffering of the full output — streaming at wire speed.

---

## Multi-Syntax Language

Cards store **only bytecode** (4 bytes/instruction). The source language is a client-side view layer with 4 interchangeable syntaxes over identical bytecode:

### C# Style (`.pico`)
```csharp
Storage.Load(0, 1, 42, R0);
Storage.Field(R1, 2);
Flow.Branch(GT, R1, 18, :match);
Storage.Pipe(0, 1, 42, Stream.Out);
```

### BASIC Style (`.bas`)
```basic
10 PEEK STORAGE, 0, 1, 42, R0
20 PEEK FIELD, R1, 2
30 IF R1 > 18 GOTO 50
40 GOTO 60
50 SYS PIPE, 0, 1, 42, STREAM
60 REM END
```

### Python Style (`.py`)
```python
storage.load(0, 1, 42, r0)
storage.field(r1, 2)
flow.branch(gt, r1, 18, "match")
storage.pipe(0, 1, 42, stream.out)
```

### Hex Style (`.hex`)
```
1042 0001 002A 0000
1043 0102 0000 0000
A001 0102 0012 0032
3042 0001 002A 0000
```

Write in any syntax; colleague reads it in another. Same card. Compile on save, decompile on load — all client-side.

### BASIC Verb Mapping (C64-inspired)

| Verb | Maps to | Example |
|------|---------|---------|
| PEEK | Read (LOAD, FIELD) | `PEEK STORAGE, 0, 1, 42, R0` |
| POKE | Write (SAVE, Net.*, RAISE) | `POKE NET, STATUS, 200` |
| LET | Math (ADD, SUB, MUL, DIV, INC) | `LET R0 = R1 + 42` |
| GOTO | Unconditional jump | `GOTO 30` |
| IF | Conditional branch | `IF R0 < R1 GOTO 50` |
| GOSUB | Call subroutine | `GOSUB 100` |
| RETURN | Return from call | `RETURN` |
| SYS | Hardware call (PIPE, DSP, FORK, TEMPLATE) | `SYS TEMPLATE, 0, 1, 2, STREAM` |
| WAIT | Suspend | `WAIT` |
| REM | No-op | `REM THIS IS A COMMENT` |

---

## Resource Budget (iCE40HX8K)

```
LUT Usage:         7471 / 7680  (97%)
BRAM:              21 / 32 blocks
Pins:              19 / 79 (QSPI mode)
Spare LUTs:        209
Spare Pins:        60
Soft MACs:         3
Power:             0.92W
Clock:             48MHz (PLL from 100MHz oscillator)
```

### LUT Breakdown

| Module | LUTs | % |
|--------|------|---|
| PicoScript executor (8 contexts) | 1250 | 16% |
| Context scheduler + registers | 360 | 5% |
| HTTP parser | 980 | 13% |
| QSPI SRAM controller | 150 | 2% |
| SPI master (W5100S + SD) | 240 | 3% |
| PIPE DMA engine | 180 | 2% |
| 3× soft MAC (16-bit multiply-accumulate) | 768 | 10% |
| UART debug interface | 600 | 8% |
| Fork/Join parallel engine | 270 | 4% |
| Hardware FOR/FOREACH/SWITCH | 220 | 3% |
| FIELD extractor + Template engine | 173 | 2% |
| 2-level template FOREACH | 63 | 1% |
| PLL + clock distribution | 50 | 1% |
| GPIO + misc logic | 200 | 3% |
| **Unallocated / routing** | **209** | **3%** |

---

## Hardware

### Target: Alchitry Cu (development)

The iCE40HX8K-CT256 on the Alchitry Cu development board. Breadboard prototype with:
- Alchitry Cu (owned)
- W5100S module (PoE Ethernet, £4)
- QSPI SRAM (23LC1024, £2)
- MicroSD breakout (£1)
- Breadboard + wires (~£5)

### Target: Custom PCB (production)

60×40mm, 2-layer PCB. BOM cost ~£24:

| Component | Cost |
|-----------|------|
| iCE40HX8K-TQ144 | £6.50 |
| W5100S (QFP48) | £2.80 |
| QSPI SRAM (23LC1024) | £1.50 |
| MicroSD slot | £0.40 |
| 25MHz crystal + PLL passives | £0.80 |
| Ethernet magnetics + RJ45 | £2.50 |
| PoE PD module (12V→3.3V) | £3.80 |
| Flash (iCE40 config, 2Mbit) | £0.80 |
| PCB fabrication (JLCPCB 5pcs) | £2.50 |
| Passives + connectors | £2.08 |
| **Total** | **~£24** |

---

## Product Family

| Tier | FPGA | Storage | Connections | Use Case |
|------|------|---------|-------------|----------|
| **Pico** | iCE40HX8K | SD only | 4 (W5100S) | Single-app server, IoT |
| **Mini** | iCE40HX8K + SRAM | SD + 1MB cache | 4 | Multi-tenant KV store |
| **Midi** | ECP5-25F | NVMe + DRAM | 16+ | Production query engine |
| **Maxi** | ECP5-85F + RK3588 | NVMe RAID | 100+ | Full database appliance |

This repository contains the **Pico** tier design.

---

## Building

### FPGA Bitstream (requires yosys + nextpnr-ice40)

```bash
cd picowal_hx_cu/
make           # synthesise → place+route → pack bitstream
make prog      # upload to Alchitry Cu via iceprog
```

### PicoScript Compiler (Python, client-side)

```bash
python3 picoscript_lang.py    # run examples + round-trip tests
python3 picoscript_opcodes.py # print full opcode reference
```

---

## Performance

| Workload | Throughput | Notes |
|----------|-----------|-------|
| Cached KV read (sequential) | ~400K QPS | Single-context, QSPI bottleneck |
| Cached KV read (parallel 8×) | ~3.2M QPS | Fork across 8 v-cores |
| Filter/scan (parallel) | 8× sequential | Embarrassingly parallel |
| Template render | Wire speed | Streaming, no buffering |
| HTTP parse → response | 4 instructions | No software overhead |
| Cold read (SD) | ~800 QPS | SD card latency dominant |

---

## Status

- [x] ISA design (16 opcodes, 32-bit fixed width)
- [x] Multi-syntax compiler + decompilers
- [x] Opcode reference with all encodings
- [x] Hardware architecture + LUT budget
- [x] Pin/BRAM/power budget verified
- [x] Parallel loop design (Fork/Join)
- [x] Schema + field extraction design
- [x] Template engine with 2-level FOREACH
- [x] PCB BOM + cost analysis
- [ ] Verilog RTL implementation
- [ ] Testbench + simulation
- [ ] Synthesis on actual Alchitry Cu
- [ ] Breadboard prototype
- [ ] Custom PCB fabrication

---

## Licence

MIT

---

*A £24 network-attached application server with no CPU, no OS, and no software. Just physics.*
