#include "isa.h"
#include "card_cache.h"
#include "packet.h"
#include <string.h>

// ============================================================
// PicoCluster VM — Core interpreter for RP2350 @ 450 MHz
// ============================================================

// Forward declarations for platform-specific I/O
extern bool pio_ring_recv(uint8_t ring_id, uint8_t *buf, uint32_t *len);
extern bool pio_ring_peek(uint8_t ring_id, uint8_t *buf, uint32_t *len);
extern void pio_ring_send(uint8_t ring_id, const uint8_t *buf, uint32_t len);
extern void pio_ring_flush(uint8_t ring_id);
extern void dma_start(uint8_t channel, void *dest, const void *src, uint32_t len);
extern bool dma_busy(uint8_t channel);
extern uint32_t get_cycle_count(void);
extern uint8_t  get_node_id(void);

// --- VM helpers ---

static inline void vm_set_flags(vm_context_t *ctx, int32_t result) {
    ctx->regs[REG_FLAGS] &= ~(FLAG_Z | FLAG_N);
    if (result == 0) ctx->regs[REG_FLAGS] |= FLAG_Z;
    if (result < 0)  ctx->regs[REG_FLAGS] |= FLAG_N;
}

static inline uint8_t *vm_data_ptr(vm_context_t *ctx, uint32_t addr) {
    if (addr >= ctx->data_size) return NULL;
    return ctx->data_base + addr;
}

static inline void vm_push(vm_context_t *ctx, uint32_t val) {
    if (ctx->sp + 4 <= ctx->stack_size) {
        *(uint32_t *)(ctx->stack_base + ctx->sp) = val;
        ctx->sp += 4;
    } else {
        ctx->state = VM_ERROR;
    }
}

static inline uint32_t vm_pop(vm_context_t *ctx) {
    if (ctx->sp >= 4) {
        ctx->sp -= 4;
        return *(uint32_t *)(ctx->stack_base + ctx->sp);
    }
    ctx->state = VM_ERROR;
    return 0;
}

// --- CRC16 CCITT implementation ---

uint16_t crc16_ccitt(const uint8_t *data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// --- Field extraction helpers ---
// Record format: [field_count:2][field_id:2][field_len:2][data...]...

static bool field_extract(const uint8_t *record, uint32_t record_len,
                          uint16_t field_idx, uint8_t **out_ptr, uint16_t *out_len) {
    if (record_len < 2) return false;
    uint16_t count;
    memcpy(&count, record, 2);
    if (field_idx >= count) return false;

    uint32_t pos = 2;
    for (uint16_t i = 0; i < count && pos + 4 <= record_len; i++) {
        uint16_t fid, flen;
        memcpy(&fid, record + pos, 2);
        memcpy(&flen, record + pos + 2, 2);
        pos += 4;
        if (i == field_idx) {
            if (pos + flen > record_len) return false;
            *out_ptr = (uint8_t *)(record + pos);
            *out_len = flen;
            return true;
        }
        pos += flen;
    }
    return false;
}

// --- Main VM execute loop ---

void vm_init(vm_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = VM_HALTED;
}

void vm_load_card(vm_context_t *ctx, uint32_t *bytecode, uint32_t len_words,
                  uint8_t major, uint8_t minor) {
    ctx->card_base = bytecode;
    ctx->card_len = len_words;
    ctx->card_major = major;
    ctx->card_minor = minor;
    ctx->pc = 0;
    ctx->call_depth = 0;
    ctx->loop_depth = 0;
    ctx->cycles = 0;
    ctx->result_len = 0;
    ctx->state = VM_RUNNING;
    // R0 always zero
    ctx->regs[REG_ZERO] = 0;
}

// Execute up to max_cycles instructions. Returns number executed.
uint32_t vm_execute(vm_context_t *ctx, uint32_t max_cycles) {
    uint32_t executed = 0;

    while (ctx->state == VM_RUNNING && executed < max_cycles) {
        if (ctx->pc >= ctx->card_len) {
            ctx->state = VM_ERROR;
            break;
        }

        uint32_t instr = ctx->card_base[ctx->pc++];
        uint8_t op  = INSTR_OP(instr);
        uint8_t rd  = INSTR_RD(instr);
        uint8_t r1  = INSTR_R1(instr);
        uint8_t r2  = INSTR_R2(instr);
        int32_t imm = INSTR_IMM18(instr);

        executed++;
        ctx->cycles++;

        switch (op) {

        // === CORE ===
        case OP_NOP:
            break;

        case OP_MOV:
            ctx->regs[rd] = ctx->regs[r1];
            break;

        case OP_LOAD: {
            uint32_t addr = ctx->regs[r1] + (uint32_t)(imm & 0xFFFF);
            uint8_t *ptr = vm_data_ptr(ctx, addr);
            if (ptr) ctx->regs[rd] = *(uint32_t *)ptr;
            else ctx->state = VM_ERROR;
            break;
        }

        case OP_STORE: {
            uint32_t addr = ctx->regs[rd] + (uint32_t)(imm & 0xFFFF);
            uint8_t *ptr = vm_data_ptr(ctx, addr);
            if (ptr) *(uint32_t *)ptr = ctx->regs[r1];
            else ctx->state = VM_ERROR;
            break;
        }

        case OP_LDI:
            ctx->regs[rd] = (uint32_t)imm;
            break;

        case OP_LDH:
            ctx->regs[rd] = (ctx->regs[rd] & 0x3FFF) | ((uint32_t)(imm & 0x3FFFF) << 14);
            break;

        case OP_PUSH:
            vm_push(ctx, ctx->regs[r1]);
            break;

        case OP_POP:
            ctx->regs[rd] = vm_pop(ctx);
            break;

        // === ALU ===
        case OP_ADD:
            ctx->regs[rd] = ctx->regs[r1] + ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_SUB:
            ctx->regs[rd] = ctx->regs[r1] - ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_MUL:
            ctx->regs[rd] = ctx->regs[r1] * ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_DIV:
            if (ctx->regs[r2] == 0) { ctx->state = VM_ERROR; break; }
            ctx->regs[rd] = ctx->regs[r1] / ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_MOD:
            if (ctx->regs[r2] == 0) { ctx->state = VM_ERROR; break; }
            ctx->regs[rd] = ctx->regs[r1] % ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_AND:
            ctx->regs[rd] = ctx->regs[r1] & ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_OR:
            ctx->regs[rd] = ctx->regs[r1] | ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_XOR:
            ctx->regs[rd] = ctx->regs[r1] ^ ctx->regs[r2];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_SHL:
            ctx->regs[rd] = ctx->regs[r1] << (ctx->regs[r2] & 31);
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_SHR:
            ctx->regs[rd] = ctx->regs[r1] >> (ctx->regs[r2] & 31);
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        case OP_CMP: {
            int32_t diff = (int32_t)ctx->regs[rd] - (int32_t)ctx->regs[r1];
            vm_set_flags(ctx, diff);
            if ((uint32_t)ctx->regs[rd] < (uint32_t)ctx->regs[r1])
                ctx->regs[REG_FLAGS] |= FLAG_C;
            break;
        }

        case OP_NOT:
            ctx->regs[rd] = ~ctx->regs[r1];
            vm_set_flags(ctx, (int32_t)ctx->regs[rd]);
            break;

        // === FLOW ===
        case OP_JMP:
            ctx->pc = (uint32_t)(imm & 0x3FFFF);
            break;

        case OP_JNZ:
            if (ctx->regs[rd] != 0)
                ctx->pc = (uint32_t)(imm & 0x3FFFF);
            break;

        case OP_JZ:
            if (ctx->regs[rd] == 0)
                ctx->pc = (uint32_t)(imm & 0x3FFFF);
            break;

        case OP_CALL:
            if (ctx->call_depth >= MAX_CALL_DEPTH) { ctx->state = VM_ERROR; break; }
            ctx->call_stack[ctx->call_depth++] = ctx->pc;
            ctx->pc = (uint32_t)(imm & 0x3FFFF);
            break;

        case OP_RET:
            if (ctx->call_depth == 0) { ctx->state = VM_ERROR; break; }
            ctx->pc = ctx->call_stack[--ctx->call_depth];
            break;

        case OP_SWITCH: {
            uint32_t index = ctx->regs[rd];
            uint32_t table_start = (uint32_t)(imm & 0x3FFFF);
            if (table_start + index < ctx->card_len) {
                ctx->pc = ctx->card_base[table_start + index] & 0x3FFFF;
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        case OP_HALT:
            ctx->state = VM_HALTED;
            break;

        case OP_YIELD:
            ctx->state = VM_YIELDED;
            break;

        // === LOOPS ===
        case OP_FOR: {
            if (ctx->loop_depth >= MAX_LOOP_DEPTH) { ctx->state = VM_ERROR; break; }
            loop_entry_t *lp = &ctx->loop_stack[ctx->loop_depth++];
            lp->counter = ctx->regs[rd];
            lp->loop_pc = ctx->pc;  // Body starts here
            lp->flags = 0;          // FOR type
            if (lp->counter == 0) {
                // Skip loop body — jump to end offset
                ctx->pc = (uint32_t)(imm & 0x3FFFF);
                ctx->loop_depth--;
            }
            break;
        }

        case OP_NEXT: {
            if (ctx->loop_depth == 0) { ctx->state = VM_ERROR; break; }
            loop_entry_t *lp = &ctx->loop_stack[ctx->loop_depth - 1];
            lp->counter--;
            ctx->regs[REG_BIDX]++;
            if (lp->counter > 0) {
                ctx->pc = lp->loop_pc;
            } else {
                ctx->loop_depth--;
            }
            break;
        }

        case OP_FOREACH: {
            if (ctx->loop_depth >= MAX_LOOP_DEPTH) { ctx->state = VM_ERROR; break; }
            loop_entry_t *lp = &ctx->loop_stack[ctx->loop_depth++];
            lp->collection = ctx->regs[r1];
            lp->counter = 0;
            lp->loop_pc = ctx->pc;
            lp->item_size = (uint16_t)(imm & 0xFFFF);
            lp->flags = 1;  // FOREACH type
            // Load first item pointer into Rd
            ctx->regs[rd] = lp->collection;
            break;
        }

        case OP_BREAK:
            if (ctx->loop_depth > 0) {
                ctx->loop_depth--;
                // Find end — stored in the FOR instruction's imm field
                // For simplicity, BREAK jumps to instruction after NEXT
                // The compiler must set this up correctly
            }
            break;

        // === DATA ===
        case OP_FIELD: {
            uint8_t *record = vm_data_ptr(ctx, ctx->regs[r1]);
            if (!record) { ctx->state = VM_ERROR; break; }
            uint8_t *fptr; uint16_t flen;
            uint16_t field_idx = (uint16_t)(imm & 0xFFFF);
            // Get record length from context (stored at regs[r1]-4 or known)
            if (field_extract(record, ctx->data_size - ctx->regs[r1],
                              field_idx, &fptr, &flen)) {
                // Store field value (up to 4 bytes) or pointer
                if (flen <= 4) {
                    ctx->regs[rd] = 0;
                    memcpy(&ctx->regs[rd], fptr, flen);
                } else {
                    // Store pointer to field data
                    ctx->regs[rd] = (uint32_t)(fptr - ctx->data_base);
                }
            } else {
                ctx->regs[rd] = 0;
            }
            break;
        }

        case OP_SETF: {
            // Simplified: set field value at record
            uint8_t *record = vm_data_ptr(ctx, ctx->regs[rd]);
            if (!record) { ctx->state = VM_ERROR; break; }
            uint16_t field_idx = (uint16_t)(imm & 0xFFFF);
            // Find field and write value from Rs
            uint8_t *fptr; uint16_t flen;
            if (field_extract(record, ctx->data_size - ctx->regs[rd],
                              field_idx, &fptr, &flen)) {
                uint32_t val = ctx->regs[r1];
                memcpy(fptr, &val, flen < 4 ? flen : 4);
            }
            break;
        }

        case OP_LEN: {
            // Buffer length — stored as uint32 at (ptr - 4) by convention
            uint32_t addr = ctx->regs[r1];
            if (addr >= 4) {
                uint8_t *ptr = vm_data_ptr(ctx, addr - 4);
                if (ptr) ctx->regs[rd] = *(uint32_t *)ptr;
                else ctx->regs[rd] = 0;
            } else {
                ctx->regs[rd] = 0;
            }
            break;
        }

        case OP_COPY: {
            uint8_t *dst = vm_data_ptr(ctx, ctx->regs[rd]);
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint32_t len = ctx->regs[r2];
            if (dst && src && (ctx->regs[rd] + len <= ctx->data_size) &&
                (ctx->regs[r1] + len <= ctx->data_size)) {
                memmove(dst, src, len);
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        case OP_FILL: {
            uint8_t *dst = vm_data_ptr(ctx, ctx->regs[rd]);
            uint32_t len = ctx->regs[r2];
            if (dst && ctx->regs[rd] + len <= ctx->data_size) {
                memset(dst, (uint8_t)ctx->regs[r1], len);
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        case OP_CRC: {
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint32_t len = ctx->regs[r2];
            if (src && ctx->regs[r1] + len <= ctx->data_size) {
                ctx->regs[rd] = crc16_ccitt(src, len);
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        case OP_HASH: {
            // Fast FNV-1a hash
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint32_t len = ctx->regs[r2];
            if (src && ctx->regs[r1] + len <= ctx->data_size) {
                uint32_t h = 0x811C9DC5;
                for (uint32_t i = 0; i < len; i++) {
                    h ^= src[i];
                    h *= 0x01000193;
                }
                ctx->regs[rd] = h;
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        case OP_FIND: {
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint8_t needle = (uint8_t)ctx->regs[r2];
            // Search from current position, need length from LEN convention
            uint32_t len = ctx->data_size - ctx->regs[r1];
            if (src) {
                uint8_t *found = memchr(src, needle, len);
                ctx->regs[rd] = found ? (uint32_t)(found - src) : 0xFFFFFFFF;
            } else {
                ctx->state = VM_ERROR;
            }
            break;
        }

        // === STRING/TEMPLATE ===
        case OP_TMPL: {
            // Template expansion: scan template for {{N}}, replace with field N from data
            uint8_t *out = vm_data_ptr(ctx, ctx->regs[rd]);
            uint8_t *tmpl = vm_data_ptr(ctx, ctx->regs[r1]);
            uint8_t *data = vm_data_ptr(ctx, ctx->regs[r2]);
            if (!out || !tmpl || !data) { ctx->state = VM_ERROR; break; }
            // Simplified streaming template
            uint32_t out_pos = 0;
            uint32_t tmpl_len = ctx->data_size - ctx->regs[r1]; // approximate
            for (uint32_t i = 0; i < tmpl_len && tmpl[i] != 0; i++) {
                if (tmpl[i] == '{' && i + 3 < tmpl_len && tmpl[i+1] == '{') {
                    // Parse field index
                    uint16_t fidx = 0;
                    i += 2;
                    while (i < tmpl_len && tmpl[i] >= '0' && tmpl[i] <= '9') {
                        fidx = fidx * 10 + (tmpl[i] - '0');
                        i++;
                    }
                    if (i < tmpl_len && tmpl[i] == '}') i++; // skip }}
                    if (i < tmpl_len && tmpl[i] == '}') ; // consume second }
                    // Extract field and copy to output
                    uint8_t *fptr; uint16_t flen;
                    if (field_extract(data, ctx->data_size - ctx->regs[r2],
                                      fidx, &fptr, &flen)) {
                        if (out_pos + flen < ctx->data_size - ctx->regs[rd]) {
                            memcpy(out + out_pos, fptr, flen);
                            out_pos += flen;
                        }
                    }
                } else {
                    if (out_pos < ctx->data_size - ctx->regs[rd]) {
                        out[out_pos++] = tmpl[i];
                    }
                }
            }
            // Store output length
            if (ctx->regs[rd] >= 4) {
                uint8_t *len_ptr = vm_data_ptr(ctx, ctx->regs[rd] - 4);
                if (len_ptr) *(uint32_t *)len_ptr = out_pos;
            }
            break;
        }

        case OP_CONCAT: {
            uint8_t *dst = vm_data_ptr(ctx, ctx->regs[rd]);
            uint8_t *a = vm_data_ptr(ctx, ctx->regs[r1]);
            uint8_t *b = vm_data_ptr(ctx, ctx->regs[r2]);
            if (!dst || !a || !b) { ctx->state = VM_ERROR; break; }
            // Read lengths (stored at ptr-4)
            uint32_t alen = (ctx->regs[r1] >= 4) ?
                *(uint32_t *)(ctx->data_base + ctx->regs[r1] - 4) : 0;
            uint32_t blen = (ctx->regs[r2] >= 4) ?
                *(uint32_t *)(ctx->data_base + ctx->regs[r2] - 4) : 0;
            if (ctx->regs[rd] + alen + blen <= ctx->data_size) {
                memcpy(dst, a, alen);
                memcpy(dst + alen, b, blen);
                if (ctx->regs[rd] >= 4) {
                    *(uint32_t *)(ctx->data_base + ctx->regs[rd] - 4) = alen + blen;
                }
            }
            break;
        }

        case OP_SLICE: {
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            if (!src) { ctx->state = VM_ERROR; break; }
            uint16_t start = (uint16_t)((imm >> 8) & 0xFF);
            uint16_t end = (uint16_t)(imm & 0xFF);
            if (end > start) {
                ctx->regs[rd] = ctx->regs[r1] + start;
                // Update length at rd-4
                if (ctx->regs[rd] >= 4) {
                    uint8_t *len_ptr = vm_data_ptr(ctx, ctx->regs[rd] - 4);
                    if (len_ptr) *(uint32_t *)len_ptr = end - start;
                }
            }
            break;
        }

        case OP_PACK: {
            // Pack fields into wire format — simplified
            // Just copy record as-is for now
            uint8_t *dst = vm_data_ptr(ctx, ctx->regs[rd]);
            uint8_t *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint32_t len = ctx->regs[r2];
            if (dst && src && len <= ctx->data_size) {
                memcpy(dst, src, len);
            }
            break;
        }

        // === I/O NATIVE ===
        case OP_RECV: {
            uint8_t ring_id = (uint8_t)(imm & 0xFF);
            uint32_t len = 0;
            uint8_t *buf = vm_data_ptr(ctx, ctx->regs[rd]);
            if (buf) {
                if (!pio_ring_recv(ring_id, buf, &len)) {
                    ctx->state = VM_WAITING;
                }
                // Store received length
                if (ctx->regs[rd] >= 4) {
                    *(uint32_t *)(ctx->data_base + ctx->regs[rd] - 4) = len;
                }
            }
            break;
        }

        case OP_SEND: {
            uint8_t ring_id = (uint8_t)(imm & 0xFF);
            uint8_t *buf = vm_data_ptr(ctx, ctx->regs[rd]);
            uint32_t len = (ctx->regs[rd] >= 4) ?
                *(uint32_t *)(ctx->data_base + ctx->regs[rd] - 4) : 0;
            if (buf && len > 0) {
                pio_ring_send(ring_id, buf, len);
            }
            break;
        }

        case OP_DMA: {
            void *dst = vm_data_ptr(ctx, ctx->regs[rd]);
            void *src = vm_data_ptr(ctx, ctx->regs[r1]);
            uint32_t len = ctx->regs[r2];
            if (dst && src) {
                dma_start(0, dst, src, len);
                ctx->regs[REG_FLAGS] |= FLAG_DMA;
            }
            break;
        }

        case OP_DWAIT:
            if (dma_busy((uint8_t)ctx->regs[rd])) {
                ctx->pc--;  // Re-execute this instruction next cycle
                ctx->state = VM_YIELDED;
            } else {
                ctx->regs[REG_FLAGS] &= ~FLAG_DMA;
            }
            break;

        case OP_PEEK: {
            uint8_t ring_id = (uint8_t)(imm & 0xFF);
            uint32_t len = 0;
            uint8_t *buf = vm_data_ptr(ctx, ctx->regs[rd]);
            if (buf && !pio_ring_peek(ring_id, buf, &len)) {
                ctx->regs[rd] = 0;  // Nothing available
            }
            break;
        }

        case OP_FLUSH: {
            uint8_t ring_id = (uint8_t)(imm & 0xFF);
            pio_ring_flush(ring_id);
            break;
        }

        // === BATCH ===
        case OP_BNEXT: {
            ctx->regs[REG_BIDX]++;
            if (ctx->regs[REG_BIDX] >= ctx->batch_count) {
                ctx->state = VM_HALTED;  // Batch complete
            } else {
                // Load next item pointer into R1
                uint32_t offset = ctx->regs[REG_BIDX] * ctx->batch_item_size;
                ctx->regs[1] = (uint32_t)(ctx->batch_base - ctx->data_base) + offset;
            }
            break;
        }

        case OP_BDONE:
            ctx->state = VM_HALTED;
            ctx->regs[REG_FLAGS] |= FLAG_BATCH;
            break;

        case OP_BLEN:
            ctx->regs[rd] = ctx->batch_count - ctx->regs[REG_BIDX];
            break;

        case OP_BITEM: {
            uint32_t idx = ctx->regs[r1];
            if (idx < ctx->batch_count) {
                uint32_t offset = idx * ctx->batch_item_size;
                ctx->regs[rd] = (uint32_t)(ctx->batch_base - ctx->data_base) + offset;
            } else {
                ctx->regs[rd] = 0;
            }
            break;
        }

        // === SYSTEM ===
        case OP_WAIT:
            // TODO: integrate with event system
            ctx->state = VM_WAITING;
            break;

        case OP_RAISE:
            // TODO: software interrupt dispatch
            break;

        case OP_TIME:
            ctx->regs[rd] = get_cycle_count();
            break;

        case OP_MYID:
            ctx->regs[rd] = get_node_id();
            break;

        case OP_STATUS:
            // Set node status for heartbeat reporting
            break;

        case OP_DEBUG:
            // Emit debug value — platform-specific
            break;

        default:
            ctx->state = VM_ERROR;
            break;
        }

        // Enforce R0 = 0 invariant
        ctx->regs[REG_ZERO] = 0;
    }

    return executed;
}
