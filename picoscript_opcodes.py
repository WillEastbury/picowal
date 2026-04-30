#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""picoscript_opcodes.py -- Complete PicoScript Opcode Reference

Full documentation of every opcode with:
  - Binary encoding
  - C# syntax
  - BASIC syntax
  - Execution behaviour
  - Cycle count
  - Which hardware block handles it

Includes the PARALLEL LOOP extension (FORK/JOIN) that distributes
loop iterations across v-cores for concurrent execution.
"""


# ═══════════════════════════════════════════════════════════════════════
# OPCODE TABLE
# ═══════════════════════════════════════════════════════════════════════
#
# 32-bit instruction word:
#   [31:28] = opcode (4-bit, 16 opcodes)
#   [27:24] = Rd     (destination register, 0-15)
#   [23:20] = Rs1    (source register 1, 0-15)
#   [19:16] = Rs2    (source register 2 / mode / condition / DSP sub-op)
#   [15:0]  = imm16  (immediate value / card address / branch offset)
#
# Registers: R0-R14 general purpose, R15 = connection context (read-only)
#
# ═══════════════════════════════════════════════════════════════════════

OPCODES = {
    0x0: {
        "mnemonic": "NOOP",
        "name": "No Operation / HTTP Control",
        "csharp": "Thread.Skip();",
        "basic": "THREAD SKIP",
        "encoding": "0000 0000 0000 0000 iiiiiiiiiiiiiiii",
        "fields": "imm16 = HTTP control word (if bit 15 set) or 0",
        "cycles": 1,
        "unit": "scheduler",
        "description": """
            Does nothing for one cycle. Used for timing alignment.
            ALSO used for Net.* HTTP control (overloaded via imm16 bit 15):
              imm16[15]=1, [14:12]=0x0 → Net.Status(code)
              imm16[15]=1, [14:12]=0x2 → Net.Type(content-type)
              imm16[15]=1, [14:12]=0x3 → Net.Body()
              imm16[15]=1, [14:12]=0x4 → Net.Close()
            The hardware HTTP framer intercepts these before they reach
            the execution pipeline — zero cycle cost for HTTP framing.
        """,
        "net_examples": {
            "Net.Status(200);":              "10 NET STATUS, 200",
            "Net.Status(404);":              "10 NET STATUS, 404",
            'Net.Type("text/html");':        '10 NET TYPE, TEXT/HTML',
            'Net.Type("application/json");': '10 NET TYPE, APPLICATION/JSON',
            "Net.Body();":                   "10 NET BODY",
            "Net.Close();":                  "10 NET CLOSE",
        },
    },

    0x1: {
        "mnemonic": "LOAD",
        "name": "Load Card to Register",
        "csharp": "Storage.Load(tenant, pack, card, Rd);",
        "basic": "STORAGE LOAD, tenant, pack, card, Rd",
        "encoding": "0001 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=dest, Rs1=indirect src, Rs2=addr mode, imm16=card addr",
        "cycles": "2 (SRAM) / 10 (QSPI) / ~48000 (SD cold)",
        "unit": "memory controller",
        "description": """
            Loads a card's content from storage into register Rd.
            Address modes:
              mode 0: immediate  — card at address imm16
              mode 1: register   — card at address [Rs1]
              mode 2: base+off   — card at address BASE+imm16
              mode 3: reg+off    — card at address [Rs1]+imm16
            If card is in SRAM cache: 2 cycles.
            If QSPI SRAM: ~10 cycles.
            If SD card (cache miss): suspends context, resumes on DMA complete.
        """,
    },

    0x2: {
        "mnemonic": "SAVE",
        "name": "Save Register to Card",
        "csharp": "Storage.Save(tenant, pack, card, Rs);",
        "basic": "STORAGE SAVE, tenant, pack, card, Rs",
        "encoding": "0010 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=source reg, Rs2=addr mode, imm16=card addr",
        "cycles": "2 (SRAM) / 10 (QSPI) / ~48000 (SD)",
        "unit": "memory controller",
        "description": """
            Writes register Rd content back to card storage.
            Same addressing modes as LOAD.
            Write goes to SRAM cache first (write-back to SD on eviction).
        """,
    },

    0x3: {
        "mnemonic": "PIPE",
        "name": "Pipe Card to Stream",
        "csharp": "Storage.Pipe(tenant, pack, card, Stream.Out);",
        "basic": "STORAGE PIPE, tenant, pack, card, STREAM",
        "encoding": "0011 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=stream(0=TCP TX), Rs2=addr mode, imm16=card addr",
        "cycles": "variable (card size / SPI bandwidth)",
        "unit": "PIPE engine + SPI master",
        "description": """
            Zero-copy fetch+emit: reads card from storage and streams it
            directly to the TCP output buffer (W5100S TX).
            No register involvement — data flows SRAM→SPI without touching
            the execution pipeline.
            Card size determines cycle count (~20 cycles per 16-bit word).
            Suspends context until transfer complete.
        """,
    },

    0x4: {
        "mnemonic": "ADD",
        "name": "Add",
        "csharp": "Math.Add(Rd, Rs1, value);",
        "basic": "MATH ADD, Rd, Rs1, value",
        "encoding": "0100 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=dest, Rs1=src, Rs2=mode(0=imm,1=reg), imm16=value",
        "cycles": 1,
        "unit": "ALU",
        "description": """
            Rd = Rs1 + imm16    (mode 0, immediate)
            Rd = Rs1 + [Rs2]    (mode 1, register — imm16 is reg number)
            Sets Z flag if result is zero. Sets carry flag on overflow.
        """,
    },

    0x5: {
        "mnemonic": "SUB",
        "name": "Subtract",
        "csharp": "Math.Sub(Rd, Rs1, value);",
        "basic": "MATH SUB, Rd, Rs1, value",
        "encoding": "0101 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=dest, Rs1=src, Rs2=mode, imm16=value",
        "cycles": 1,
        "unit": "ALU",
        "description": """
            Rd = Rs1 - imm16  (mode 0) or Rd = Rs1 - Rx (mode 1).
            Sets Z flag, sets negative flag if result < 0.
        """,
    },

    0x6: {
        "mnemonic": "MUL",
        "name": "Multiply",
        "csharp": "Math.Mul(Rd, Rs1, value);",
        "basic": "MATH MUL, Rd, Rs1, value",
        "encoding": "0110 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=dest, Rs1=src, Rs2=mode, imm16=value",
        "cycles": "8 (soft shift-add) / 1 (ECP5 coprocessor)",
        "unit": "ALU (soft MAC)",
        "description": """
            Rd = Rs1 × imm16 (mode 0) or Rd = Rs1 × Rx (mode 1).
            16-bit × 16-bit → 32-bit result.
            Without coprocessor: shift-add loop, 8 cycles.
            With ECP5 coprocessor: dispatched, 1 cycle + 2 overhead.
        """,
    },

    0x7: {
        "mnemonic": "DIV",
        "name": "Divide",
        "csharp": "Math.Div(Rd, Rs1, value);",
        "basic": "MATH DIV, Rd, Rs1, value",
        "encoding": "0111 dddd ssss mmmm iiiiiiiiiiiiiiii",
        "fields": "Rd=dest(quotient), Rs1=dividend, imm16=divisor",
        "cycles": "32 (soft restoring) / 3 (ECP5)",
        "unit": "ALU (soft divider)",
        "description": """
            Rd = Rs1 / imm16 (mode 0) or Rd = Rs1 / Rx (mode 1).
            Integer division, remainder discarded (use MOD for remainder).
            Division by zero sets ERR flag and Rd = 0xFFFF.
        """,
    },

    0x8: {
        "mnemonic": "INC",
        "name": "Increment",
        "csharp": "Math.Inc(Rd);",
        "basic": "MATH INC, Rd",
        "encoding": "1000 dddd 0000 0000 0000000000000000",
        "fields": "Rd=register to increment",
        "cycles": 1,
        "unit": "ALU",
        "description": """
            Rd = Rd + 1.
            Sets Z flag if result wraps to zero.
            Exists as separate opcode because loop counters are the #1 use case.
        """,
    },

    0x9: {
        "mnemonic": "JUMP",
        "name": "Unconditional Jump",
        "csharp": "Flow.Jump(:label);",
        "basic": "FLOW JUMP, target",
        "encoding": "1001 0000 0000 0000 iiiiiiiiiiiiiiii",
        "fields": "imm16 = absolute target PC",
        "cycles": 1,
        "unit": "branch unit",
        "description": """
            PC = imm16. Unconditional.
            Used for: infinite loops, goto, skip blocks.
        """,
    },

    0xA: {
        "mnemonic": "BRANCH",
        "name": "Conditional Branch",
        "csharp": "Flow.Branch(condition, Ra, Rb, :label);",
        "basic": "FLOW BRANCH, cond, Ra, Rb, target",
        "encoding": "1010 aaaa bbbb cccc iiiiiiiiiiiiiiii",
        "fields": "Rd=Ra, Rs1=Rb, Rs2=condition, imm16=relative offset",
        "cycles": 1,
        "unit": "branch unit",
        "description": """
            If condition(Ra, Rb) is true: PC = PC + sign_extend(imm16).
            Otherwise: PC = PC + 1 (fall through).

            Conditions (Rs2 field):
              0x0 EQ   Ra == Rb
              0x1 NE   Ra != Rb
              0x2 LT   Ra <  Rb  (signed)
              0x3 GT   Ra >  Rb  (signed)
              0x4 LE   Ra <= Rb  (signed)
              0x5 GE   Ra >= Rb  (signed)
              0x6 Z    Ra == 0   (Rb ignored)
              0x7 NZ   Ra != 0   (Rb ignored)
              0x8 EOF  end-of-stream flag set
              0x9 ERR  error flag set

            LOOPS are just BRANCH backwards:
              Math.Inc(R0);
              Flow.Branch(LT, R0, R1, :loop_top);  // offset = negative
        """,
    },

    0xB: {
        "mnemonic": "CALL",
        "name": "Call Subroutine (card)",
        "csharp": "Flow.Call(:label);",
        "basic": "FLOW CALL, target",
        "encoding": "1011 0000 0000 0000 iiiiiiiiiiiiiiii",
        "fields": "imm16 = target PC (or card address for cross-card calls)",
        "cycles": 1,
        "unit": "branch unit + call stack",
        "description": """
            Push current PC+1 onto call stack, then PC = imm16.
            Call stack is 8 deep (hardware limit).
            Can call into another card (imm16 = card address) — the PC
            within that card starts at 0.
        """,
    },

    0xC: {
        "mnemonic": "RETURN",
        "name": "Return from Subroutine",
        "csharp": "Flow.Return();",
        "basic": "FLOW RETURN",
        "encoding": "1100 0000 0000 0000 0000000000000000",
        "fields": "none",
        "cycles": 1,
        "unit": "branch unit + call stack",
        "description": """
            Pop call stack, PC = popped value.
            If call stack is empty: terminates the connection context
            (sends TCP FIN, releases context slot for next connection).
        """,
    },

    0xD: {
        "mnemonic": "WAIT",
        "name": "Wait for Interrupt",
        "csharp": "Thread.Wait();",
        "basic": "THREAD WAIT",
        "encoding": "1101 0000 0000 0000 iiiiiiiiiiiiiiii",
        "fields": "imm16 = interrupt mask (which channels to wait on)",
        "cycles": "0 (suspends context until woken)",
        "unit": "scheduler + IRQ controller",
        "description": """
            Suspends the current v-core context.
            Context is removed from round-robin scheduling.
            Woken when a matching RAISE fires (imm16 = channel mask).
            Used for: event-driven patterns, pub/sub, long-poll HTTP.
            Other contexts continue executing while this one sleeps.
        """,
    },

    0xE: {
        "mnemonic": "RAISE",
        "name": "Raise Software Interrupt",
        "csharp": "Thread.Raise(channel);",
        "basic": "THREAD RAISE, channel",
        "encoding": "1110 0000 0000 0000 iiiiiiiiiiiiiiii",
        "fields": "imm16 = interrupt channel (0-15)",
        "cycles": 1,
        "unit": "IRQ controller",
        "description": """
            Fires a software interrupt on the specified channel.
            Any context WAITing on that channel mask is woken.
            Used for: inter-context signalling, producer/consumer patterns,
            notifying a blocked context that new data is available.
        """,
    },

    0xF: {
        "mnemonic": "DSP",
        "name": "DSP / AI Accelerator Operation",
        "csharp": "Dsp.MatMul(Rd, Rs1);",
        "basic": "DSP MATMUL, Rd, Rs1",
        "encoding": "1111 dddd ssss oooo iiiiiiiiiiiiiiii",
        "fields": "Rd=dest, Rs1=src, Rs2=DSP sub-op, imm16=param",
        "cycles": "8-2048 (depends on operation and data size)",
        "unit": "soft MAC array / ECP5 coprocessor",
        "description": """
            Dispatches to DSP sub-operation (Rs2 field selects which):
              0x0 MATMUL    Matrix multiply (Rd = Rs1 × SRAM matrix)
              0x1 SOFTMAX   Softmax activation (Rd = softmax(Rs1))
              0x2 DOT       Dot product (Rd = Rs1 · imm16_ptr)
              0x3 SCALE     Scale vector (Rd = Rs1 × imm16 / 256)
              0x4 RELU      ReLU activation (Rd = max(0, Rs1))
              0x5 NORM      Layer normalisation
              0x6 TOPK      Top-K selection (imm16 = K)
              0x7 GELU      GELU activation (approx)
              0x8 TRANSPOSE Matrix transpose
              0x9 VADD      Vector add (Rd = Rd + Rs1)
              0xA EMBED     Embedding lookup
              0xB QUANT     Quantize (float→int8)
              0xC DEQUANT   Dequantize (int8→float)
              0xD MASK      Apply attention mask
              0xE CONCAT    Concatenate vectors
              0xF SPLIT     Split vector into heads

            Without ECP5: executed in soft logic (slow but functional).
            With ECP5: dispatched via coprocessor bus (fast).
        """,
    },
}


# ═══════════════════════════════════════════════════════════════════════
# PARALLEL LOOP EXTENSION (uses THREAD namespace + special imm16)
# ═══════════════════════════════════════════════════════════════════════
#
# Parallel loops DON'T need a new opcode. They use WAIT/RAISE + context
# scheduling to distribute work across v-cores. But for ergonomics, we
# add syntactic sugar that the compiler expands:
#
# Source (C# style):
#   Thread.Fork(R0, R1, :loop_body);   // fork R0..R1 iterations across v-cores
#   :loop_body
#   Storage.Load(0, 3, 0, R3);        // each v-core gets different R0 value
#   Storage.Pipe(0, 3, 0, R3);
#   Thread.Join();                      // barrier: wait for all v-cores done
#
# Source (BASIC style):
#   10 THREAD FORK, R0, R1, 30
#   20 REM --- loop body runs on all v-cores ---
#   30 STORAGE LOAD, 0, 3, 0, R3
#   40 STORAGE PIPE, 0, 3, 0, R3
#   50 THREAD JOIN
#
# Compiles to (bytecode):
#   RAISE with special imm16 encoding (bit 15 = fork signal)
#   ... loop body (executed by N v-cores, each with unique R0) ...
#   WAIT with special imm16 encoding (bit 15 = join barrier)
#
# The fork controller:
#   1. Sets iteration counter = R0 (start)
#   2. Wakes all idle v-cores, each gets next counter value in their R0
#   3. Each v-core executes loop body independently
#   4. Each v-core hits JOIN → marks itself done, suspends
#   5. When all v-cores done → wakes the forking context
#
# Cost: 270 LUTs for fork/join controller
# Benefit: 8× throughput for embarrassingly parallel loops (e.g. filter)

PARALLEL_OPS = {
    "Fork": {
        "csharp": "Thread.Fork(Rstart, Rend, :body);",
        "basic": "SYS FORK, Rstart, Rend, target",
        "encoding": "RAISE with imm16 bit 15 set + fork descriptor",
        "description": """
            Distributes iterations [Rstart..Rend) across available v-cores.
            Each v-core receives a unique iteration value in R0.
            The loop body (at :body) executes independently on each v-core.
            Forking context suspends until JOIN completes.
        """,
        "cycles": "~4 (fork setup) + body×(iterations/v-cores)",
    },
    "Join": {
        "csharp": "Thread.Join();",
        "basic": "WAIT JOIN",
        "encoding": "WAIT with imm16 bit 15 set (join barrier)",
        "description": """
            Barrier synchronisation. Each v-core that hits JOIN marks itself
            done and suspends. When ALL forked v-cores have joined, the
            original forking context resumes at the instruction after Fork.
        """,
        "cycles": "0 (suspends until all done)",
    },
}


# ═══════════════════════════════════════════════════════════════════════
# LOOP PATTERNS (how loops work with these opcodes)
# ═══════════════════════════════════════════════════════════════════════

LOOP_PATTERNS = """
═══════════════════════════════════════════════════════════════════════
LOOP PATTERNS -- How to write loops in PicoScript
═══════════════════════════════════════════════════════════════════════

1. COUNTED LOOP (for i = start to end)
─────────────────────────────────────────────────────────────────────

   C# style:
     Math.Add(R0, R0, 0);              // R0 = 0 (counter)
     Math.Add(R1, R1, 100);            // R1 = 100 (limit)
     :loop
     // ... loop body using R0 as index ...
     Math.Inc(R0);                     // R0++
     Flow.Branch(LT, R0, R1, :loop);  // if R0 < R1, goto loop

   BASIC style:
     10 MATH ADD, R0, R0, 0
     20 MATH ADD, R1, R1, 100
     30 REM --- LOOP BODY ---
     40 MATH INC, R0
     50 FLOW BRANCH, LT, R0, R1, 30

   Bytecode:
     [0] 40000000    ADD R0, R0, 0
     [1] 41100064    ADD R1, R1, 100
     [2] ...         (loop body)
     [3] 80000000    INC R0
     [4] A0140002    BRANCH LT, R0, R1, -2 (relative to loop body)


2. WHILE LOOP (while condition)
─────────────────────────────────────────────────────────────────────

   C# style:
     :check
     Flow.Branch(GE, R0, R1, :done);  // while R0 < R1
     // ... loop body ...
     Flow.Jump(:check);
     :done

   BASIC style:
     10 FLOW BRANCH, GE, R0, R1, 50
     20 REM --- LOOP BODY ---
     30 FLOW JUMP, 10
     40 REM --- DONE ---


3. DO-WHILE (always executes at least once)
─────────────────────────────────────────────────────────────────────

   C# style:
     :top
     // ... loop body ...
     Flow.Branch(NZ, R0, R0, :top);   // repeat while R0 != 0

   BASIC style:
     10 REM --- LOOP BODY ---
     20 FLOW BRANCH, NZ, R0, R0, 10


4. ITERATE OVER CARDS (for each card in range)
─────────────────────────────────────────────────────────────────────

   C# style:
     Math.Add(R0, R0, 100);            // start card addr
     Math.Add(R1, R1, 200);            // end card addr
     :next_card
     Storage.Load(0, 0, 0, R2);        // R2 = card[R0] (indirect via mode 1)
     // ... process R2 ...
     Math.Inc(R0);
     Flow.Branch(LT, R0, R1, :next_card);

   BASIC style:
     10 MATH ADD, R0, R0, 100
     20 MATH ADD, R1, R1, 200
     30 STORAGE LOAD, 0, 0, 0, R2
     40 REM --- PROCESS R2 ---
     50 MATH INC, R0
     60 FLOW BRANCH, LT, R0, R1, 30


5. PARALLEL LOOP (fork across v-cores) ★ KILLER FEATURE ★
─────────────────────────────────────────────────────────────────────

   C# style:
     Math.Add(R0, R0, 0);              // start = 0
     Math.Add(R1, R1, 1000);           // end = 1000
     Thread.Fork(R0, R1, :body);       // distribute 0..999 across 8 v-cores
     :body
     Storage.Load(0, 3, 0, R2);        // each v-core: R0 = unique iteration
     Flow.Branch(LE, R2, R5, :skip);   // filter: only emit if > threshold
     Storage.Pipe(0, 3, 0, R0);
     :skip
     Thread.Join();                     // barrier: all v-cores sync here

   BASIC style:
     10 MATH ADD, R0, R0, 0
     20 MATH ADD, R1, R1, 1000
     30 THREAD FORK, R0, R1, 50
     40 REM --- BODY RUNS ON ALL 8 V-CORES ---
     50 STORAGE LOAD, 0, 3, 0, R2
     60 FLOW BRANCH, LE, R2, R5, 80
     70 STORAGE PIPE, 0, 3, 0, R0
     80 THREAD JOIN

   Execution:
     - Fork distributes iterations: v-core0 gets R0=0..124,
       v-core1 gets R0=125..249, ... v-core7 gets R0=875..999
     - All 8 v-cores execute body simultaneously
     - Each v-core processes 125 cards independently
     - JOIN waits for all 8 to finish
     - Sequential: 1000 iterations × ~3 cycles = 3000 cycles
     - Parallel:   125 iterations × ~3 cycles = 375 cycles (8× speedup!)

   Works because:
     - V-cores have independent register banks (BRAM)
     - V-cores share SRAM read access (time-multiplexed)
     - No data dependency between iterations
     - Fork/Join is just RAISE/WAIT with extra scheduling logic


6. INFINITE LOOP (server pattern)
─────────────────────────────────────────────────────────────────────

   C# style:
     :serve
     Thread.Wait();                     // sleep until connection arrives
     // ... handle request ...
     Flow.Return();                     // close connection
     Flow.Jump(:serve);                 // (never reached if Return closes)

   BASIC style:
     10 THREAD WAIT
     20 REM --- HANDLE REQUEST ---
     30 FLOW RETURN
     40 FLOW JUMP, 10

   Note: In practice, each connection gets its own v-core context.
   The "infinite loop" is really the context scheduler re-assigning
   the v-core to the next connection after RETURN.
"""


# ═══════════════════════════════════════════════════════════════════════
# LUT BUDGET IMPACT (QSPI + parallel loops)
# ═══════════════════════════════════════════════════════════════════════

REBALANCED_BUDGET = """
═══════════════════════════════════════════════════════════════════════
REBALANCED LUT BUDGET -- Full Feature Set (Final)
═══════════════════════════════════════════════════════════════════════

Evolution: 5 MACs + parallel SRAM → 3 MACs + QSPI + parallel loops
         + hardware loops + schema/field + template engine + eMMC

REMOVED (from original 7420 LUT build):
  - 2× soft MAC (256×16 multipliers):            -512 LUTs
  - Parallel SRAM controller (async):             -250 LUTs
  Total freed:                                    -762 LUTs

ADDED:
  + QSPI SRAM controller (quad-SPI, mode 0):     +150 LUTs
  + Fork controller (iteration distributor):       +80 LUTs
  + Join barrier (done flags + wake logic):        +40 LUTs
  + Parallel reduce (collect results):            +100 LUTs
  + Shared iteration counter (atomic inc):         +50 LUTs
  + Hardware FOR (counter + auto-branch):          +80 LUTs
  + Hardware FOREACH (card iterator + auto-load): +100 LUTs
  + Hardware SWITCH (BRAM jump table):             +40 LUTs
  + Schema FIELD extractor (barrel shift + type):  +80 LUTs
  + Template engine (marker detect + stream mux):  +75 LUTs
  + Template 2-level FOREACH (nesting state):      +63 LUTs
  Total added:                                    +858 LUTs

NET CHANGE: -762 + 858 = +96 LUTs (slightly more than before)

FINAL BUDGET:
  Original total:                                 7420 LUTs
  Net change:                                      -762 + 858 = +96
  New total:                                      7471 LUTs (97%)
  Spare:                                           209 LUTs

PIN BUDGET (with optional eMMC):
  Core (always):  52 pins (SRAM+W5100S+SD+UART)
  Optional eMMC:   7 pins (CLK+CMD+DAT[3:0]+DET#)
  Total used:     59 / 79
  Spare:          20 pins

BRAM:            21 / 32 blocks
Soft MACs:        3 (was 5 — traded 2 for templates + parallel loops)
Power:           0.92W (USB powered)
Clock:           48MHz

STORAGE HIERARCHY (3-tier, L2 optional):
  L1: QSPI SRAM     128KB-1MB    200ns      always present
  L2: eMMC (4-bit)  4-32GB       ~100µs     auto-detected at boot
  L3: SD card (SPI) 2-32GB       ~1ms       always present

  Detection: DET# pin pulled HIGH (10K). eMMC module grounds it.
  At boot: sample DET# — LOW=enable L2, HIGH=skip (2-tier mode).
"""


# ═══════════════════════════════════════════════════════════════════════
# HARDWARE EXTENSIONS (special addressing modes, not new opcodes)
# ═══════════════════════════════════════════════════════════════════════

HARDWARE_EXTENSIONS = {
    "FOR": {
        "opcode_host": "BRANCH (0xA)",
        "mode": "Rs2 = 0x4 (hardware loop mode)",
        "csharp": "Flow.For(Rcounter, Rlimit, :body);",
        "basic": "FOR R0 = 0 TO R1",
        "encoding": "1010 cccc llll 0100 bbbbbbbbbbbbbbbb",
        "fields": "Rd=counter reg, Rs1=limit reg, Rs2=0x4(FOR mode), imm16=body start offset",
        "cycles": "0 overhead per iteration (counter decrement + branch is free)",
        "luts": 80,
        "description": """
            Zero-overhead hardware counted loop. The loop counter decrements
            and branches back in the SAME cycle as the last instruction of the
            body — no wasted cycles. Counter in dedicated hardware register.
            Compiler always emits this for counted loops.
        """,
    },
    "FOREACH": {
        "opcode_host": "LOAD (0x1)",
        "mode": "Rs2 = 0x4 (iterate mode)",
        "csharp": "Storage.ForEach(tenant, pack, :body);",
        "basic": "FOREACH CARD IN pack",
        "encoding": "0001 dddd pppp 0100 bbbbbbbbbbbbbbbb",
        "fields": "Rd=current card reg, Rs1=pack id, Rs2=0x4(FOREACH mode), imm16=body offset",
        "cycles": "auto-loads each card (QSPI latency per card, iteration is free)",
        "luts": 100,
        "description": """
            Hardware card iterator. Walks every card in a pack sequentially.
            Auto-increments card pointer, auto-loads next card into Rd, raises
            EOF flag when pack exhausted. Body executes once per card with
            current card data available in Rd. Compiler always emits this.
        """,
    },
    "SWITCH": {
        "opcode_host": "JUMP (0x9)",
        "mode": "Rs2 = 0x1 (indexed jump mode)",
        "csharp": "Flow.Switch(Rindex, :jump_table);",
        "basic": "SWITCH R0",
        "encoding": "1001 0000 iiii 0001 tttttttttttttttttt",
        "fields": "Rs1=index reg, Rs2=0x1(SWITCH mode), imm16=table base in BRAM",
        "cycles": "1 (single BRAM lookup + jump)",
        "luts": 40,
        "description": """
            O(1) computed jump via BRAM lookup table. Index register selects
            entry from jump table stored in BRAM. Result is target IP.
            Replaces N sequential BRANCH comparisons with 1 table read.
            Bounds-checked: index > table_size → default case.
        """,
    },
    "FIELD": {
        "opcode_host": "LOAD (0x1)",
        "mode": "Rs2 = 0x5 (field extract mode)",
        "csharp": "Storage.Field(Rd, field_id);",
        "basic": "PEEK FIELD, Rd, field_id",
        "encoding": "0001 dddd ffff 0101 ssssssssssssssss",
        "fields": "Rd=dest reg, Rs1=field_id (0-15), Rs2=0x5(FIELD mode), imm16=schema_id",
        "cycles": "1 (BRAM schema lookup + barrel shift)",
        "luts": 80,
        "description": """
            Extracts a typed field from the currently loaded card into Rd.
            Schema table (in BRAM) maps field_id → (offset, type, length).
            Hardware barrel shifter aligns the field bytes into the register.
            Type-aware: u8 zero-extends, u16 zero-extends, u32 direct,
            str returns pointer to first byte.
            Enables hardware WHERE clauses: FIELD + BRANCH = filter.
        """,
    },
    "TEMPLATE": {
        "opcode_host": "PIPE (0x3)",
        "mode": "Rs2 = 0x2 (template mode)",
        "csharp": "Storage.Template(tmpl_card, data_card, child_pack, grandchild_pack, Stream.Out);",
        "basic": "SYS TEMPLATE, tmpl, data, children, grandchildren, STREAM",
        "encoding": "0011 tttt dddd 0010 cccccccccccccccc",
        "fields": "Rd=template card, Rs1=data card, Rs2=0x2(TEMPLATE mode), imm16=child pack(hi8)+grandchild pack(lo8)",
        "cycles": "variable (streams at wire speed, proportional to output size)",
        "luts": 138,
        "description": """
            Streaming template renderer with 2-level nested FOREACH.
            Reads template card byte-by-byte toward TCP TX buffer.
            Special marker bytes trigger inline substitution:
              0xFE + field_id  → replace with field value from data card
              0xFD + pack_id   → begin FOREACH (push level, iterate pack)
              0xFC             → end FOREACH (loop back or pop level)
            Supports 2 nesting levels (master → child → grandchild).
            Entire HTML page renders in one instruction, streaming.
            No output buffering — data flows directly to network.
        """,
    },
}


# ═══════════════════════════════════════════════════════════════════════
# BASIC VERB MAPPING (C64-inspired mnemonics)
# ═══════════════════════════════════════════════════════════════════════

BASIC_VERBS = {
    "REM": {
        "maps_to": ["NOOP (opcode 0x0)"],
        "example": "REM THIS IS A COMMENT",
        "description": "No-op. Line is ignored. Maps to NOOP with imm16=0.",
    },
    "PEEK": {
        "maps_to": ["LOAD (opcode 0x1)", "FIELD (LOAD mode 5)"],
        "examples": [
            "PEEK STORAGE, 0, 1, 42, R0    → LOAD card 42 from tenant 0, pack 1 into R0",
            "PEEK FIELD, R0, 2             → extract field 2 from current card into R0",
        ],
        "description": "Read from storage or extract field. C64 PEEK = read memory.",
    },
    "POKE": {
        "maps_to": ["SAVE (opcode 0x2)", "RAISE (opcode 0xE)", "Net.* (NOOP + HTTP)"],
        "examples": [
            "POKE STORAGE, 0, 1, 42, R0    → SAVE R0 to card 42",
            "POKE NET, STATUS, 200         → set HTTP status 200",
            "POKE NET, TYPE, JSON          → set content-type application/json",
            "POKE NET, BODY               → begin body",
            "POKE NET, CLOSE              → close connection",
            "POKE THREAD, 3               → RAISE interrupt on channel 3",
        ],
        "description": "Write to storage, network, or signal. C64 POKE = write memory.",
    },
    "LET": {
        "maps_to": ["ADD (0x4)", "SUB (0x5)", "MUL (0x6)", "DIV (0x7)", "INC (0x8)"],
        "examples": [
            "LET R0 = R1 + 42              → ADD R0, R1, 42",
            "LET R0 = R1 - 1               → SUB R0, R1, 1",
            "LET R0 = R1 * 8               → MUL R0, R1, 8",
            "LET R0 = R1 / 4               → DIV R0, R1, 4",
            "LET R0 = R0 + 1               → INC R0",
        ],
        "description": "Math assignment. C64 LET = assign variable. Operator determines opcode.",
    },
    "GOTO": {
        "maps_to": ["JUMP (opcode 0x9)"],
        "examples": [
            "GOTO 30                        → JUMP to line 30 (local, within card)",
            "GOTO CARD, 0, 5, 12            → JUMP to tenant 0, pack 5, card 12",
        ],
        "description": "Unconditional jump. Mode 0 = local (intra-card), mode 1 = cross-card.",
    },
    "IF": {
        "maps_to": ["BRANCH (opcode 0xA)"],
        "examples": [
            "IF R0 < R1 GOTO 50             → BRANCH LT, R0, R1, line 50",
            "IF R0 = 0 GOTO 80              → BRANCH Z, R0, _, line 80",
            "IF R0 > 18 GOTO CARD, 0, 2, 5  → BRANCH GT, R0, 18, card(0,2,5)",
        ],
        "description": "Conditional branch. Condition: = < > <= >= <> Z NZ EOF ERR.",
    },
    "GOSUB": {
        "maps_to": ["CALL (opcode 0xB)"],
        "examples": [
            "GOSUB 100                      → CALL line 100 (local)",
            "GOSUB CARD, 0, 3, 1           → CALL to card (pushes current card+IP)",
        ],
        "description": "Subroutine call. Pushes (pack, card, IP+1) onto call stack.",
    },
    "RETURN": {
        "maps_to": ["RETURN (opcode 0xC)"],
        "example": "RETURN",
        "description": "Pop (pack, card, IP) from call stack, resume there.",
    },
    "WAIT": {
        "maps_to": ["WAIT (opcode 0xD)"],
        "examples": [
            "WAIT                           → suspend until any RAISE",
            "WAIT JOIN                      → barrier (parallel loop end)",
        ],
        "description": "Suspend context. Woken by RAISE from another context or hardware.",
    },
    "SYS": {
        "maps_to": ["PIPE (opcode 0x3)", "DSP (opcode 0xF)", "TEMPLATE (PIPE mode 2)", "FORK (RAISE+fork)"],
        "examples": [
            "SYS PIPE, 0, 1, 42, STREAM    → PIPE card to TCP output",
            "SYS TEMPLATE, tmpl, data, children, STREAM  → render template",
            "SYS DSP, MATMUL, R0, R1       → matrix multiply",
            "SYS FORK, R0, R1, 30          → parallel fork over range",
        ],
        "description": "Hardware system call. Invokes DMA, DSP, template, or fork engines.",
    },
    "FOR": {
        "maps_to": ["BRANCH mode 4 (hardware FOR)"],
        "examples": [
            "FOR R0 = 0 TO 100             → hardware zero-overhead counted loop",
            "NEXT R0                       → (implicit, handled by hardware)",
        ],
        "description": "Hardware counted loop. Zero overhead — branch is free.",
    },
    "FOREACH": {
        "maps_to": ["LOAD mode 4 (hardware FOREACH)"],
        "examples": [
            "FOREACH R0 IN PACK 5           → iterate all cards in pack 5",
            "NEXT                           → (implicit, hardware auto-advances)",
        ],
        "description": "Hardware card iterator. Auto-loads each card, EOF when done.",
    },
}


# ═══════════════════════════════════════════════════════════════════════
# Main (print full reference)
# ═══════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    print("PicoScript Opcode Reference v1.0")
    print("=" * 65)
    print()
    print("16 opcodes × 4-bit decode = single LUT layer on FPGA")
    print("32-bit fixed width: [opcode:4][Rd:4][Rs1:4][Rs2:4][imm16:16]")
    print()

    for code, op in sorted(OPCODES.items()):
        print(f"{'─'*65}")
        print(f"  0x{code:X}  {op['mnemonic']:8s}  {op['name']}")
        print(f"{'─'*65}")
        print(f"  Encoding: {op['encoding']}")
        print(f"  Fields:   {op['fields']}")
        print(f"  Cycles:   {op['cycles']}")
        print(f"  Unit:     {op['unit']}")
        print()
        print(f"  C# style:    {op['csharp']}")
        print(f"  BASIC style: {op['basic']}")
        print()
        if "net_examples" in op:
            print("  HTTP variants:")
            for cs, bas in op["net_examples"].items():
                print(f"    {cs:40s} → {bas}")
            print()
        # Print first 3 lines of description
        desc = op["description"].strip().split("\n")
        for line in desc[:6]:
            print(f"  {line.strip()}")
        if len(desc) > 6:
            print(f"  ...")
        print()

    # Parallel extension
    print()
    print("═" * 65)
    print("PARALLEL LOOP EXTENSION (Fork/Join)")
    print("═" * 65)
    for name, op in PARALLEL_OPS.items():
        print(f"\n  Thread.{name}")
        print(f"  C# style:    {op['csharp']}")
        print(f"  BASIC style: {op['basic']}")
        print(f"  Cycles:      {op['cycles']}")
        desc = op["description"].strip().split("\n")
        for line in desc:
            print(f"    {line.strip()}")

    # Loop patterns
    print()
    print(LOOP_PATTERNS)

    # Budget impact
    print(REBALANCED_BUDGET)

    # Hardware extensions
    print()
    print("═" * 65)
    print("HARDWARE EXTENSIONS (special addressing modes)")
    print("═" * 65)
    for name, ext in HARDWARE_EXTENSIONS.items():
        print(f"\n  {name}")
        print(f"  Host opcode: {ext['opcode_host']}")
        print(f"  Mode:        {ext['mode']}")
        print(f"  C# style:    {ext['csharp']}")
        print(f"  BASIC style: {ext['basic']}")
        print(f"  Encoding:    {ext['encoding']}")
        print(f"  Cycles:      {ext['cycles']}")
        print(f"  LUTs:        {ext['luts']}")
        desc = ext["description"].strip().split("\n")
        for line in desc:
            print(f"    {line.strip()}")

    # BASIC verbs
    print()
    print("═" * 65)
    print("BASIC VERB MAPPING (C64-inspired)")
    print("═" * 65)
    for verb, info in BASIC_VERBS.items():
        print(f"\n  {verb}")
        print(f"    Maps to: {', '.join(info['maps_to'])}")
        if "example" in info:
            print(f"    Example: {info['example']}")
        if "examples" in info:
            for ex in info["examples"][:3]:
                print(f"    {ex}")
        print(f"    {info['description']}")
