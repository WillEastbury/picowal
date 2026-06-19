#ifndef PICOCLUSTER_ISA_H
#define PICOCLUSTER_ISA_H

#include <stdint.h>

// ============================================================
// PicoCluster ISA — 32-bit fixed-width, 6-bit opcode, 16 regs
// ============================================================

// --- Opcode definitions ---

// CORE (0x00-0x07)
#define OP_NOP      0x00
#define OP_MOV      0x01
#define OP_LOAD     0x02
#define OP_STORE    0x03
#define OP_LDI      0x04
#define OP_LDH      0x05
#define OP_PUSH     0x06
#define OP_POP      0x07

// ALU (0x08-0x13)
#define OP_ADD      0x08
#define OP_SUB      0x09
#define OP_MUL      0x0A
#define OP_DIV      0x0B
#define OP_MOD      0x0C
#define OP_AND      0x0D
#define OP_OR       0x0E
#define OP_XOR      0x0F
#define OP_SHL      0x10
#define OP_SHR      0x11
#define OP_CMP      0x12
#define OP_NOT      0x13

// FLOW (0x14-0x1B)
#define OP_JMP      0x14
#define OP_JNZ      0x15
#define OP_JZ       0x16
#define OP_CALL     0x17
#define OP_RET      0x18
#define OP_SWITCH   0x19
#define OP_HALT     0x1A
#define OP_YIELD    0x1B

// LOOPS (0x1C-0x1F)
#define OP_FOR      0x1C
#define OP_NEXT     0x1D
#define OP_FOREACH  0x1E
#define OP_BREAK    0x1F

// DATA (0x20-0x27)
#define OP_FIELD    0x20
#define OP_SETF     0x21
#define OP_LEN      0x22
#define OP_COPY     0x23
#define OP_FILL     0x24
#define OP_CRC      0x25
#define OP_HASH     0x26
#define OP_FIND     0x27

// STRING/TEMPLATE (0x28-0x2B)
#define OP_TMPL     0x28
#define OP_CONCAT   0x29
#define OP_SLICE    0x2A
#define OP_PACK     0x2B

// I/O NATIVE (0x2C-0x31)
#define OP_RECV     0x2C
#define OP_SEND     0x2D
#define OP_DMA      0x2E
#define OP_DWAIT    0x2F
#define OP_PEEK     0x30
#define OP_FLUSH    0x31

// BATCH (0x32-0x35)
#define OP_BNEXT    0x32
#define OP_BDONE    0x33
#define OP_BLEN     0x34
#define OP_BITEM    0x35

// SYSTEM (0x36-0x3B)
#define OP_WAIT     0x36
#define OP_RAISE    0x37
#define OP_TIME     0x38
#define OP_MYID     0x39
#define OP_STATUS   0x3A
#define OP_DEBUG    0x3B

// RESERVED (0x3C-0x3F)
#define OP_RSVD0    0x3C
#define OP_RSVD1    0x3D
#define OP_RSVD2    0x3E
#define OP_RSVD3    0x3F

// --- Instruction decode macros ---

#define INSTR_OP(instr)     ((uint8_t)((instr) >> 26))
#define INSTR_RD(instr)     ((uint8_t)(((instr) >> 22) & 0xF))
#define INSTR_R1(instr)     ((uint8_t)(((instr) >> 18) & 0xF))
#define INSTR_R2(instr)     ((uint8_t)(((instr) >> 14) & 0xF))
#define INSTR_IMM18(instr)  ((int32_t)(((instr) & 0x3FFFF) | \
                            (((instr) & 0x20000) ? 0xFFFC0000 : 0)))
#define INSTR_FLAGS(instr)  ((uint16_t)((instr) & 0x3FFF))

// --- Instruction encode macros ---

#define ENCODE_RI(op, rd, rs, imm) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rd) << 22) | \
     ((uint32_t)(rs) << 18) | ((uint32_t)((imm) & 0x3FFFF)))

#define ENCODE_RRR(op, rd, r1, r2, flags) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rd) << 22) | \
     ((uint32_t)(r1) << 18) | ((uint32_t)(r2) << 14) | \
     ((uint32_t)((flags) & 0x3FFF)))

// --- Register aliases ---

#define REG_ZERO    0
#define REG_BIDX    13   // Batch index
#define REG_BCNT    14   // Batch count
#define REG_FLAGS   15   // Status flags

// --- FLAGS bits ---

#define FLAG_Z      (1 << 0)  // Zero
#define FLAG_N      (1 << 1)  // Negative
#define FLAG_C      (1 << 2)  // Carry
#define FLAG_V      (1 << 3)  // Overflow
#define FLAG_BATCH  (1 << 4)  // Batch active
#define FLAG_DMA    (1 << 5)  // DMA busy

// --- Addressing modes (bits 17:16 of imm18) ---

#define ADDR_IMM      0x00  // Immediate value
#define ADDR_REG_IND  0x01  // [Rs + offset]
#define ADDR_STK_REL  0x02  // [SP + offset]
#define ADDR_PC_REL   0x03  // PC + offset

#define ADDR_MODE(imm18)   (((imm18) >> 16) & 0x3)
#define ADDR_OFFSET(imm18) ((int16_t)((imm18) & 0xFFFF))

// --- VM state ---

typedef enum {
    VM_RUNNING = 0,
    VM_HALTED,
    VM_WAITING,
    VM_YIELDED,
    VM_ERROR
} vm_state_t;

// Loop stack entry
typedef struct {
    uint32_t counter;      // Remaining iterations
    uint32_t loop_pc;      // PC of loop body start
    uint32_t collection;   // For FOREACH: pointer to collection
    uint16_t item_size;    // For FOREACH: size of each item
    uint16_t flags;        // Loop type (FOR vs FOREACH)
} loop_entry_t;

#define MAX_LOOP_DEPTH  8
#define MAX_CALL_DEPTH  32

// VM execution context
typedef struct {
    uint32_t    regs[16];               // Register file
    uint32_t    pc;                     // Program counter (word index)
    uint32_t    call_stack[MAX_CALL_DEPTH];
    uint8_t     call_depth;
    loop_entry_t loop_stack[MAX_LOOP_DEPTH];
    uint8_t     loop_depth;
    vm_state_t  state;
    uint16_t    card_major;
    uint16_t    card_minor;
    uint32_t    cycles;                 // Instruction counter
    // Pointers to memory regions (set at init)
    uint32_t   *card_base;             // Current card bytecode
    uint32_t    card_len;              // Card length in words
    uint8_t    *data_base;             // Working data memory
    uint32_t    data_size;             // Data region size
    uint8_t    *stack_base;            // Stack memory
    uint32_t    stack_size;
    uint32_t    sp;                    // Stack pointer (byte offset)
    // Batch state
    uint8_t    *batch_base;            // Batch input buffer
    uint32_t    batch_item_size;
    uint32_t    batch_count;
    // Result output
    uint8_t    *result_buf;
    uint32_t    result_len;
    uint32_t    result_capacity;
} vm_context_t;

#endif // PICOCLUSTER_ISA_H
