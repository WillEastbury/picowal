# PicoCluster ISA Reference

## Overview

32-bit fixed-width instruction set for the PicoScript VM running on RP2350 @ 450 MHz.
16 general-purpose registers, 6-bit opcode (64 slots), 18-bit immediate field.

## Instruction Formats

### 2-Register + Immediate (R-I)
```
[31:26] op (6 bits)
[25:22] Rd (4 bits)
[21:18] Rs (4 bits)
[17:0]  imm18 (18 bits, sign-extended)
```

### 3-Register (R-R-R)
```
[31:26] op (6 bits)
[25:22] Rd (4 bits)
[21:18] R1 (4 bits)
[17:14] R2 (4 bits)
[13:0]  flags/unused (14 bits)
```

## Register File

| Register | Name   | Purpose                          |
|----------|--------|----------------------------------|
| R0       | ZERO   | Hardwired zero                   |
| R1-R12   | GP     | General purpose                  |
| R13      | BIDX   | Batch index (auto-inc by NEXT)   |
| R14      | BCNT   | Batch count                      |
| R15      | FLAGS  | Status/comparison flags          |

### FLAGS register (R15) bits:
```
Bit 0: Zero flag (Z)
Bit 1: Negative flag (N)
Bit 2: Carry flag (C)
Bit 3: Overflow flag (V)
Bit 4: Batch active
Bit 5: DMA busy
```

## Addressing Modes (imm18 bits [17:16])

| Mode | Encoding | Meaning                      |
|------|----------|------------------------------|
| 00   | IMM      | Immediate value (16-bit signed) |
| 01   | REG_IND  | [Rs + offset16]              |
| 10   | STK_REL  | [SP + offset16]              |
| 11   | PC_REL   | PC + offset16 (for jumps)    |

## Opcode Map

### CORE (0x00-0x07)
| Op   | Mnemonic | Encoding    | Description                      |
|------|----------|-------------|----------------------------------|
| 0x00 | NOP      | -           | No operation                     |
| 0x01 | MOV      | Rd, Rs      | Rd = Rs                          |
| 0x02 | LOAD     | Rd, [Rs+imm]| Load 32-bit from memory          |
| 0x03 | STORE    | [Rd+imm], Rs| Store 32-bit to memory           |
| 0x04 | LDI      | Rd, imm18   | Load immediate (sign-extended)   |
| 0x05 | LDH      | Rd, imm18   | Load high (Rd = Rd | imm<<14)   |
| 0x06 | PUSH     | Rs          | Push Rs to call stack            |
| 0x07 | POP      | Rd          | Pop call stack to Rd             |

### ALU (0x08-0x13)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x08 | ADD      | Rd, R1, R2    | Rd = R1 + R2                   |
| 0x09 | SUB      | Rd, R1, R2    | Rd = R1 - R2                   |
| 0x0A | MUL      | Rd, R1, R2    | Rd = R1 * R2                   |
| 0x0B | DIV      | Rd, R1, R2    | Rd = R1 / R2 (unsigned)        |
| 0x0C | MOD      | Rd, R1, R2    | Rd = R1 % R2                   |
| 0x0D | AND      | Rd, R1, R2    | Rd = R1 & R2                   |
| 0x0E | OR       | Rd, R1, R2    | Rd = R1 | R2                   |
| 0x0F | XOR      | Rd, R1, R2    | Rd = R1 ^ R2                   |
| 0x10 | SHL      | Rd, R1, R2    | Rd = R1 << R2                  |
| 0x11 | SHR      | Rd, R1, R2    | Rd = R1 >> R2 (logical)        |
| 0x12 | CMP      | R1, R2        | Set FLAGS = compare(R1, R2)    |
| 0x13 | NOT      | Rd, Rs        | Rd = ~Rs                       |

### FLOW (0x14-0x1B)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x14 | JMP      | imm18         | PC = imm18                     |
| 0x15 | JNZ      | Rs, imm18     | if (Rs != 0) PC = imm18       |
| 0x16 | JZ       | Rs, imm18     | if (Rs == 0) PC = imm18       |
| 0x17 | CALL     | imm18         | push PC; PC = imm18           |
| 0x18 | RET      | -             | PC = pop()                     |
| 0x19 | SWITCH   | Rd, [imm]     | PC = jump_table[Rd] at imm    |
| 0x1A | HALT     | -             | Execution complete             |
| 0x1B | YIELD    | -             | Pause, resume on next tick     |

### LOOPS (0x1C-0x1F)
| Op   | Mnemonic | Encoding        | Description                    |
|------|----------|-----------------|--------------------------------|
| 0x1C | FOR      | Rd, imm18       | Begin loop: Rd=counter, imm=end offset |
| 0x1D | NEXT     | -               | Rd--, if >0 branch to FOR+1, R13++ |
| 0x1E | FOREACH  | Rd, Rs, imm     | Iterate: Rd=item, Rs=collection, imm=end |
| 0x1F | BREAK    | -               | Exit innermost loop            |

### DATA (0x20-0x27)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x20 | FIELD    | Rd, Rs, imm   | Extract field #imm from record at Rs |
| 0x21 | SETF     | Rd, Rs, imm   | Set field #imm in record Rd to Rs |
| 0x22 | LEN      | Rd, Rs         | Rd = byte length of buffer Rs  |
| 0x23 | COPY     | Rd, Rs, R2     | memcpy(Rd, Rs, R2 bytes)       |
| 0x24 | FILL     | Rd, Rs, R2     | memset(Rd, Rs_val, R2 bytes)   |
| 0x25 | CRC      | Rd, Rs, R2     | CRC16(Rs, R2 bytes) → Rd       |
| 0x26 | HASH     | Rd, Rs, R2     | fast hash(Rs, R2 bytes) → Rd   |
| 0x27 | FIND     | Rd, Rs, R2     | find byte R2 in buffer Rs → Rd |

### STRING/TEMPLATE (0x28-0x2B)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x28 | TMPL     | Rd, Rs, R2    | Template expand: out=Rd, tmpl=Rs, data=R2 |
| 0x29 | CONCAT   | Rd, R1, R2    | Append buffer R2 to R1 → Rd   |
| 0x2A | SLICE    | Rd, Rs, imm   | Rd = Rs[start:end] (imm encodes both) |
| 0x2B | PACK     | Rd, Rs, R2    | Serialize fields → wire format |

### I/O NATIVE (0x2C-0x31)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x2C | RECV     | Rd, imm       | Block until PIO data → Rd (imm=ring) |
| 0x2D | SEND     | Rs, imm       | Push buffer to PIO ring (imm=ring) |
| 0x2E | DMA      | Rd, Rs, R2    | Async DMA: dest=Rd, src=Rs, len=R2 |
| 0x2F | DWAIT    | Rd             | Wait for DMA channel Rd        |
| 0x30 | PEEK     | Rd, imm       | Non-blocking ring read (0 if empty) |
| 0x31 | FLUSH    | imm            | Flush output buffer to ring    |

### BATCH (0x32-0x35)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x32 | BNEXT    | -              | Advance R13, load next → R1   |
| 0x33 | BDONE    | -              | Signal batch complete          |
| 0x34 | BLEN     | Rd             | Rd = remaining batch items     |
| 0x35 | BITEM    | Rd, Rs         | Random access: item at index Rs |

### SYSTEM (0x36-0x3B)
| Op   | Mnemonic | Encoding      | Description                    |
|------|----------|---------------|--------------------------------|
| 0x36 | WAIT     | imm            | Sleep until event bitmask      |
| 0x37 | RAISE    | imm            | Fire software interrupt        |
| 0x38 | TIME     | Rd             | Cycle counter → Rd             |
| 0x39 | MYID     | Rd             | This node's ID → Rd            |
| 0x3A | STATUS   | Rs             | Set node status register       |
| 0x3B | DEBUG    | Rs             | Emit debug byte to monitor     |

### RESERVED (0x3C-0x3F)
Reserved for future: crypto, compress, atomics, etc.

## Memory Model

- Cards execute from SRAM (128KB cache region)
- Data stack and working memory in separate 128KB region
- LOAD/STORE address relative to data region base
- FIELD/SETF operate on record buffers (pointer in register)
- No virtual memory, no MMU — flat physical addressing within VM sandbox

## Execution Context

```
struct vm_context {
    uint32_t regs[16];       // Register file
    uint32_t pc;             // Program counter (word index into card)
    uint32_t sp;             // Stack pointer
    uint32_t loop_stack[8];  // Nested loop state (FOR/FOREACH)
    uint8_t  loop_depth;     // Current nesting level
    uint8_t  state;          // RUNNING, HALTED, WAITING, YIELDED
    uint16_t card_major;     // Current card ID
    uint16_t card_minor;
};
```

## Timing Targets (@ 450 MHz)

| Category       | Cycles | Time    |
|---------------|--------|---------|
| Simple ops    | 2-3    | ~5 ns   |
| ALU           | 3-4    | ~8 ns   |
| Branch (taken)| 3      | ~7 ns   |
| FIELD/SETF    | 5-8    | ~15 ns  |
| RECV (ready)  | 3      | ~7 ns   |
| RECV (block)  | ∞      | waits   |
| DMA kick      | 3      | ~7 ns   |
| TMPL          | 10+    | ~25 ns+ |
