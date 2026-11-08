#include "arm64_dynarec.h"
#include "memory.h"
#include "hle.h"
#ifdef _WIN32
__declspec(dllimport) void* __stdcall VirtualAlloc(void* lpAddress, size_t dwSize, unsigned long flAllocationType, unsigned long flProtect);
__declspec(dllimport) int __stdcall VirtualFree(void* lpAddress, size_t dwSize, unsigned long dwFreeType);
#else
#include <sys/mman.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

ARM64Dynarec g_dynarec = {0};

// Map x86 register index (0..7: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI) to ARM64 register number (19..26)
static const uint8_t s_x86_to_arm64_reg[8] = {
    ARM64_REG_EAX, // 0: EAX -> W19
    ARM64_REG_ECX, // 1: ECX -> W20
    ARM64_REG_EDX, // 2: EDX -> W21
    ARM64_REG_EBX, // 3: EBX -> W22
    ARM64_REG_ESP, // 4: ESP -> W23
    ARM64_REG_EBP, // 5: EBP -> W24
    ARM64_REG_ESI, // 6: ESI -> W25
    ARM64_REG_EDI  // 7: EDI -> W26
};

int arm64_dynarec_init(size_t cache_size, bool dump_jit) {
    if (g_dynarec.code_buffer) return 0;

    size_t size = cache_size ? cache_size : DEFAULT_JIT_CACHE_SIZE;

    // Allocate executable memory for ARM64 JIT cache
#ifdef _WIN32
    g_dynarec.code_buffer = (uint8_t*)VirtualAlloc(NULL, size, 0x1000 | 0x2000, 0x40);
    if (!g_dynarec.code_buffer) {
        fprintf(stderr, "VirtualAlloc ARM64 JIT code buffer failed\n");
        return -1;
    }
#else
    g_dynarec.code_buffer = (uint8_t*)mmap(NULL, size,
                                           PROT_READ | PROT_WRITE | PROT_EXEC,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_dynarec.code_buffer == MAP_FAILED) {
        perror("mmap ARM64 JIT code buffer failed");
        g_dynarec.code_buffer = NULL;
        return -1;
    }
#endif

    g_dynarec.blocks = (TranslationBlock*)calloc(JIT_MAX_BLOCKS, sizeof(TranslationBlock));
    if (!g_dynarec.blocks) {
#ifdef _WIN32
        VirtualFree(g_dynarec.code_buffer, 0, 0x8000);
#else
        munmap(g_dynarec.code_buffer, size);
#endif
        g_dynarec.code_buffer = NULL;
        return -1;
    }

    g_dynarec.code_capacity = size;
    g_dynarec.code_size = 0;
    g_dynarec.block_count = 0;
    g_dynarec.dump_jit = dump_jit;

#if defined(__aarch64__)
    g_dynarec.enabled = true;
#else
    g_dynarec.enabled = false; // Cross-translating on non-ARM64 host
#endif

    return 0;
}

void arm64_dynarec_cleanup(void) {
    if (g_dynarec.code_buffer) {
#ifdef _WIN32
        VirtualFree(g_dynarec.code_buffer, 0, 0x8000);
#else
        munmap(g_dynarec.code_buffer, g_dynarec.code_capacity);
#endif
        g_dynarec.code_buffer = NULL;
    }
    if (g_dynarec.blocks) {
        free(g_dynarec.blocks);
        g_dynarec.blocks = NULL;
    }
    g_dynarec.code_size = 0;
    g_dynarec.block_count = 0;
}

// -------------------------------------------------------------
// A64 (ARM64) Instruction Emitters
// -------------------------------------------------------------

typedef struct {
    uint32_t* ptr;
    uint32_t  count;
    uint32_t  capacity;
} A64Emitter;

static inline void a64_emit(A64Emitter* e, uint32_t insn) {
    if (e->count < e->capacity) {
        e->ptr[e->count++] = insn;
    }
}

// ADD Wd, Wn, Wm
static inline void a64_add_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x0B000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// SUB Wd, Wn, Wm
static inline void a64_sub_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x4B000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// SUBS Wd, Wn, Wm (CMP)
static inline void a64_subs_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x6B000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// ADD Wd, Wn, #imm12
static inline void a64_add_imm(A64Emitter* e, uint8_t rd, uint8_t rn, uint16_t imm12) {
    a64_emit(e, 0x11000000 | (((uint32_t)imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rd);
}

// SUB Wd, Wn, #imm12
static inline void a64_sub_imm(A64Emitter* e, uint8_t rd, uint8_t rn, uint16_t imm12) {
    a64_emit(e, 0x51000000 | (((uint32_t)imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rd);
}

// EOR Wd, Wn, Wm (XOR)
static inline void a64_eor_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x4A000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// ORR Wd, Wn, Wm
static inline void a64_orr_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x2A000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// AND Wd, Wn, Wm
static inline void a64_and_reg(A64Emitter* e, uint8_t rd, uint8_t rn, uint8_t rm) {
    a64_emit(e, 0x0A000000 | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

// MOV Wd, Wm (ORR Wd, WZR, Wm)
static inline void a64_mov_reg(A64Emitter* e, uint8_t rd, uint8_t rm) {
    a64_emit(e, 0x2A0003E0 | ((uint32_t)rm << 16) | rd);
}

// MOVZ Wd, #imm16, LSL #0
static inline void a64_movz(A64Emitter* e, uint8_t rd, uint16_t imm16, uint8_t shift) {
    a64_emit(e, 0x52800000 | (((uint32_t)shift / 16) << 21) | ((uint32_t)imm16 << 5) | rd);
}

// MOVK Wd, #imm16, LSL #shift
static inline void a64_movk(A64Emitter* e, uint8_t rd, uint16_t imm16, uint8_t shift) {
    a64_emit(e, 0x72800000 | (((uint32_t)shift / 16) << 21) | ((uint32_t)imm16 << 5) | rd);
}

// Load 32-bit constant into Wd
static inline void a64_mov_imm32(A64Emitter* e, uint8_t rd, uint32_t imm) {
    a64_movz(e, rd, (uint16_t)(imm & 0xFFFF), 0);
    if ((imm >> 16) != 0) {
        a64_movk(e, rd, (uint16_t)(imm >> 16), 16);
    }
}

// LDR Wt, [Xn, Wm, UXTW] (32-bit load from memory arena at base Xn + offset Wm)
static inline void a64_ldr_uxtw(A64Emitter* e, uint8_t wt, uint8_t xn, uint8_t wm) {
    a64_emit(e, 0xB8605800 | ((uint32_t)wm << 16) | ((uint32_t)xn << 5) | wt);
}

// STR Wt, [Xn, Wm, UXTW] (32-bit store into memory arena at base Xn + offset Wm)
static inline void a64_str_uxtw(A64Emitter* e, uint8_t wt, uint8_t xn, uint8_t wm) {
    a64_emit(e, 0xB8205800 | ((uint32_t)wm << 16) | ((uint32_t)xn << 5) | wt);
}

// RET
static inline void a64_ret(A64Emitter* e) {
    a64_emit(e, 0xD65F03C0);
}

// NOP
static inline void a64_nop(A64Emitter* e) {
    a64_emit(e, 0xD503201F);
}

// -------------------------------------------------------------
// Translation Block Lookup & JIT Compilation
// -------------------------------------------------------------

TranslationBlock* arm64_dynarec_find(uint32_t x86_pc) {
    for (uint32_t i = 0; i < g_dynarec.block_count; i++) {
        if (g_dynarec.blocks[i].x86_entry == x86_pc) {
            return &g_dynarec.blocks[i];
        }
    }
    return NULL;
}

TranslationBlock* arm64_dynarec_compile(CPUState* cpu, uint32_t x86_pc) {
    if (g_dynarec.block_count >= JIT_MAX_BLOCKS) return NULL;
    if (g_dynarec.code_size + 4096 > g_dynarec.code_capacity) {
        // Reset code cache when full
        g_dynarec.code_size = 0;
        g_dynarec.block_count = 0;
    }

    uint32_t* arm_ptr = (uint32_t*)(g_dynarec.code_buffer + g_dynarec.code_size);
    A64Emitter e = {
        .ptr = arm_ptr,
        .count = 0,
        .capacity = 1024
    };

    uint32_t cur_pc = x86_pc;
    bool block_ended = false;

    // Prologue: STP X19..X28 to stack if executing as independent function
    // For our inlined block runner: registers W19..W26 are already loaded
    while (!block_ended && (cur_pc - x86_pc) < 128) {
        uint8_t op = mem_read_u8(cur_pc++);

        // Check HLE Trap (0x0F 0x3F)
        if (op == 0x0F) {
            uint8_t op2 = mem_read_u8(cur_pc++);
            if (op2 == 0x3F) {
                // End block on HLE synthetic thunk call
                block_ended = true;
                cur_pc += 2;
                break;
            }
        }

        switch (op) {
            // NOP
            case 0x90:
                a64_nop(&e);
                break;

            // MOV reg, imm32
            case 0xB8 ... 0xBF: {
                uint8_t x86_reg = op - 0xB8;
                uint8_t arm_reg = s_x86_to_arm64_reg[x86_reg];
                uint32_t imm = mem_read_u32(cur_pc);
                cur_pc += 4;
                a64_mov_imm32(&e, arm_reg, imm);
                break;
            }

            // MOV reg, reg (0x89: mov r/m32, r32)
            case 0x89: {
                uint8_t b = mem_read_u8(cur_pc++);
                uint8_t mod = (b >> 6) & 3;
                uint8_t reg = (b >> 3) & 7;
                uint8_t rm = b & 7;
                if (mod == 3) {
                    a64_mov_reg(&e, s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[reg]);
                }
                break;
            }

            // ADD reg, reg (0x01: add r/m32, r32)
            case 0x01: {
                uint8_t b = mem_read_u8(cur_pc++);
                uint8_t mod = (b >> 6) & 3;
                uint8_t reg = (b >> 3) & 7;
                uint8_t rm = b & 7;
                if (mod == 3) {
                    a64_add_reg(&e, s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[reg]);
                }
                break;
            }

            // SUB reg, reg (0x29: sub r/m32, r32)
            case 0x29: {
                uint8_t b = mem_read_u8(cur_pc++);
                uint8_t mod = (b >> 6) & 3;
                uint8_t reg = (b >> 3) & 7;
                uint8_t rm = b & 7;
                if (mod == 3) {
                    a64_sub_reg(&e, s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[reg]);
                }
                break;
            }

            // XOR reg, reg (0x31: xor r/m32, r32)
            case 0x31: {
                uint8_t b = mem_read_u8(cur_pc++);
                uint8_t mod = (b >> 6) & 3;
                uint8_t reg = (b >> 3) & 7;
                uint8_t rm = b & 7;
                if (mod == 3) {
                    a64_eor_reg(&e, s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[rm], s_x86_to_arm64_reg[reg]);
                }
                break;
            }

            // PUSH reg
            case 0x50 ... 0x57: {
                uint8_t x86_reg = op - 0x50;
                uint8_t arm_reg = s_x86_to_arm64_reg[x86_reg];
                // sub W23, W23, #4
                a64_sub_imm(&e, ARM64_REG_ESP, ARM64_REG_ESP, 4);
                // str arm_reg, [X28, W23, UXTW]
                a64_str_uxtw(&e, arm_reg, ARM64_REG_MEM_BASE, ARM64_REG_ESP);
                break;
            }

            // POP reg
            case 0x58 ... 0x5F: {
                uint8_t x86_reg = op - 0x58;
                uint8_t arm_reg = s_x86_to_arm64_reg[x86_reg];
                // ldr arm_reg, [X28, W23, UXTW]
                a64_ldr_uxtw(&e, arm_reg, ARM64_REG_MEM_BASE, ARM64_REG_ESP);
                // add W23, W23, #4
                a64_add_imm(&e, ARM64_REG_ESP, ARM64_REG_ESP, 4);
                break;
            }

            // RET
            case 0xC3:
                block_ended = true;
                break;

            default:
                // Stop block on unhandled or control transfer instruction
                block_ended = true;
                cur_pc--; // Unconsume opcode
                break;
        }
    }

    // Epilogue: return from JIT block
    a64_ret(&e);

    uint32_t arm_bytes = e.count * sizeof(uint32_t);

    // Flush CPU Instruction Cache on ARM64 host
#if defined(__aarch64__)
    __builtin___clear_cache((char*)arm_ptr, (char*)arm_ptr + arm_bytes);
#endif

    if (g_dynarec.dump_jit) {
        printf("[ARM64 JIT] Compiled x86 [0x%08X - 0x%08X] -> %u A64 instructions (%u bytes):\n",
               x86_pc, cur_pc, e.count, arm_bytes);
        for (uint32_t i = 0; i < e.count; i++) {
            printf("   +%04X: 0x%08X\n", i * 4, arm_ptr[i]);
        }
    }

    TranslationBlock* tb = &g_dynarec.blocks[g_dynarec.block_count++];
    tb->x86_entry = x86_pc;
    tb->x86_size = cur_pc - x86_pc;
    tb->arm64_offset = g_dynarec.code_size;
    tb->arm64_size = arm_bytes;
    tb->exec_count = 0;

    g_dynarec.code_size += (arm_bytes + 15) & ~15;
    return tb;
}

int arm64_dynarec_exec(CPUState* cpu, TranslationBlock* tb) {
    if (!tb) return 0;
    tb->exec_count++;

#if defined(__aarch64__)
    // Execute native A64 translated block directly on ARM64 hardware!
    typedef void (*JITBlockFn)(uint32_t* regs, void* mem_base);
    JITBlockFn fn = (JITBlockFn)(g_dynarec.code_buffer + tb->arm64_offset);

    // Map CPUState registers into ARM64 execution context
    fn(cpu->regs, g_mem.arena);
#endif

    // Advance EIP by translated size
    cpu->eip += tb->x86_size;
    cpu->instruction_count += (tb->x86_size > 0 ? tb->x86_size : 1);
    return 1;
}
