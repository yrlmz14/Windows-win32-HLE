#include "cpu.h"
#include "memory.h"
#include "hle.h"
#include "teb_peb.h"
#include "arm64_dynarec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAG_CF 0x0001
#define FLAG_PF 0x0004
#define FLAG_AF 0x0010
#define FLAG_ZF 0x0040
#define FLAG_SF 0x0080
#define FLAG_OF 0x0800

void cpu_init(CPUState* cpu, DWORD entry_point, DWORD stack_top, DWORD fs_base) {
    memset(cpu, 0, sizeof(CPUState));
    cpu->eip = entry_point;
    cpu->esp = stack_top;
    cpu->ebp = stack_top;
    cpu->fs_base = fs_base ? fs_base : DEFAULT_TEB_ADDRESS;
    cpu->eflags = 0x202; // IF set
    cpu->halted = false;
    cpu->exit_code = 0;
    cpu->instruction_count = 0;
}

void cpu_halt(CPUState* cpu, uint32_t exit_code) {
    cpu->halted = true;
    cpu->exit_code = exit_code;
}

void cpu_set_edx(CPUState* cpu, uint32_t val) {
    cpu->edx = val;
}

static inline void push32(CPUState* cpu, uint32_t val) {
    cpu->esp -= 4;
    mem_write_u32(cpu->esp, val);
}

static inline uint32_t pop32(CPUState* cpu) {
    uint32_t val = mem_read_u32(cpu->esp);
    cpu->esp += 4;
    return val;
}

static inline void set_flags_add(CPUState* cpu, uint32_t a, uint32_t b, uint32_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if ((uint64_t)a + (uint64_t)b > 0xFFFFFFFFULL) cpu->eflags |= FLAG_CF;
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80000000) cpu->eflags |= FLAG_SF;
    if (((a ^ res) & (b ^ res)) & 0x80000000) cpu->eflags |= FLAG_OF;
}

static inline void set_flags_sub(CPUState* cpu, uint32_t a, uint32_t b, uint32_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if (a < b) cpu->eflags |= FLAG_CF;
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80000000) cpu->eflags |= FLAG_SF;
    if (((a ^ b) & (a ^ res)) & 0x80000000) cpu->eflags |= FLAG_OF;
}

static inline void set_flags_logic(CPUState* cpu, uint32_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF);
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80000000) cpu->eflags |= FLAG_SF;
}

static inline void set_flags_logic8(CPUState* cpu, uint8_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_OF | FLAG_ZF | FLAG_SF);
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80) cpu->eflags |= FLAG_SF;
}

static inline void set_flags_sub8(CPUState* cpu, uint8_t a, uint8_t b, uint8_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if (a < b) cpu->eflags |= FLAG_CF;
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80) cpu->eflags |= FLAG_SF;
    if (((a ^ b) & (a ^ res)) & 0x80) cpu->eflags |= FLAG_OF;
}

static inline void set_flags_add8(CPUState* cpu, uint8_t a, uint8_t b, uint8_t res) {
    cpu->eflags &= ~(FLAG_CF | FLAG_ZF | FLAG_SF | FLAG_OF);
    if ((uint16_t)a + (uint16_t)b > 0xFF) cpu->eflags |= FLAG_CF;
    if (res == 0) cpu->eflags |= FLAG_ZF;
    if (res & 0x80) cpu->eflags |= FLAG_SF;
    if (((a ^ res) & (b ^ res)) & 0x80) cpu->eflags |= FLAG_OF;
}

typedef struct {
    uint8_t mod;
    uint8_t reg;
    uint8_t rm;
    uint32_t eff_addr;
    bool is_reg;
} ModRM;

static ModRM decode_modrm(CPUState* cpu, uint32_t* pc, uint32_t seg_base) {
    ModRM m;
    uint8_t b = mem_read_u8((*pc)++);
    m.mod = (b >> 6) & 3;
    m.reg = (b >> 3) & 7;
    m.rm  = b & 7;
    m.is_reg = (m.mod == 3);

    if (m.is_reg) {
        m.eff_addr = 0;
        return m;
    }

    uint32_t base = 0;

    if (m.rm == 4) {
        // SIB byte
        uint8_t sib = mem_read_u8((*pc)++);
        uint8_t scale = (sib >> 6) & 3;
        uint8_t index = (sib >> 3) & 7;
        uint8_t base_reg = sib & 7;

        uint32_t idx_val = (index == 4) ? 0 : cpu->regs[index];
        base = idx_val << scale;

        if (m.mod == 0 && base_reg == 5) {
            base += mem_read_u32(*pc);
            *pc += 4;
        } else {
            base += cpu->regs[base_reg];
        }
    } else if (m.mod == 0 && m.rm == 5) {
        base = mem_read_u32(*pc);
        *pc += 4;
    } else {
        base = cpu->regs[m.rm];
    }

    if (m.mod == 1) {
        int8_t disp8 = (int8_t)mem_read_u8((*pc)++);
        base += (int32_t)disp8;
    } else if (m.mod == 2) {
        int32_t disp32 = (int32_t)mem_read_u32(*pc);
        *pc += 4;
        base += disp32;
    }

    m.eff_addr = seg_base + base;
    return m;
}

static inline uint8_t read_r8(CPUState* cpu, uint8_t reg) {
    if (reg < 4) return cpu->regs[reg] & 0xFF;
    return (cpu->regs[reg - 4] >> 8) & 0xFF;
}

static inline void write_r8(CPUState* cpu, uint8_t reg, uint8_t val) {
    if (reg < 4) cpu->regs[reg] = (cpu->regs[reg] & ~0xFF) | val;
    else cpu->regs[reg - 4] = (cpu->regs[reg - 4] & ~0xFF00) | ((uint32_t)val << 8);
}

static inline uint8_t read_rm8(CPUState* cpu, const ModRM* m) {
    if (m->is_reg) return read_r8(cpu, m->rm);
    return mem_read_u8(m->eff_addr);
}

static inline void write_rm8(CPUState* cpu, const ModRM* m, uint8_t val) {
    if (m->is_reg) write_r8(cpu, m->rm, val);
    else mem_write_u8(m->eff_addr, val);
}

static uint32_t read_rm32(CPUState* cpu, const ModRM* m) {
    if (m->is_reg) return cpu->regs[m->rm];
    return mem_read_u32(m->eff_addr);
}

static void write_rm32(CPUState* cpu, const ModRM* m, uint32_t val) {
    if (m->is_reg) cpu->regs[m->rm] = val;
    else mem_write_u32(m->eff_addr, val);
}

int cpu_step(CPUState* cpu) {
    if (cpu->halted) return 0;

    uint32_t pc = cpu->eip;
    uint32_t seg_base = 0;

    bool has_rep = false;
    bool is_op16 = false;

    // Prefixes
    while (1) {
        uint8_t prefix = mem_read_u8(pc);
        if (prefix == 0x64) { // FS: segment prefix
            seg_base = cpu->fs_base;
            pc++;
        } else if (prefix == 0xF3) { // REP
            has_rep = true;
            pc++;
        } else if (prefix == 0x66) { // 16-bit operand size
            is_op16 = true;
            pc++;
        } else if (prefix == 0x2E || prefix == 0x3E || prefix == 0x26 || prefix == 0x36 ||
                   prefix == 0x65 || prefix == 0x67 || prefix == 0xF0 || prefix == 0xF2) {
            // CS, DS, ES, SS, GS, Address size, lock, repne
            pc++;
        } else {
            break;
        }
    }

    uint8_t op = mem_read_u8(pc++);

    // 1. Check Magic HLE Trap: 0x0F 0x3F <id_low> <id_high>
    if (op == 0x0F) {
        uint8_t op2 = mem_read_u8(pc++);
        if (op2 == 0x3F) {
            uint16_t func_id = mem_read_u16(pc);
            pc += 2;

            HLEFunction* fn = hle_get_by_id(func_id);
            if (!fn) {
                fprintf(stderr, "CPU: Unknown HLE func id %u at 0x%08X\n", func_id, cpu->eip);
                cpu->halted = true;
                return -1;
            }

            if (g_hle.trace_calls) {
                printf("[HLE TRACE] %s!%s (args=%u, esp=0x%08X)\n",
                       fn->module_name, fn->function_name ? fn->function_name : "<ord>",
                       fn->num_args, cpu->esp);
            }

            // Read arguments from stack
            // Return address is at cpu->esp
            uint32_t args[16] = {0};
            for (uint8_t i = 0; i < fn->num_args && i < 16; i++) {
                args[i] = mem_read_u32(cpu->esp + 4 + (i * 4));
            }

            // Execute HLE Handler
            uint32_t ret_val = fn->handler(cpu, args);
            cpu->eax = ret_val;

            // Handle return
            uint32_t ret_addr = pop32(cpu);
            if (!fn->is_cdecl && fn->num_args > 0) {
                // stdcall: callee cleans arguments
                cpu->esp += fn->num_args * 4;
            }
            cpu->eip = ret_addr;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xB1) {
            // CMPXCHG r/m32, r32 (Atomic compare and swap)
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            if (cpu->eax == dst) {
                cpu->eflags |= FLAG_ZF;
                write_rm32(cpu, &m, cpu->regs[m.reg]);
            } else {
                cpu->eflags &= ~FLAG_ZF;
                cpu->eax = dst;
            }
            set_flags_sub(cpu, cpu->eax, dst, cpu->eax - dst);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xC1) {
            // XADD r/m32, r32 (Atomic exchange and add)
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst + src;
            write_rm32(cpu, &m, res);
            cpu->regs[m.reg] = dst;
            set_flags_add(cpu, dst, src, res);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 >= 0x90 && op2 <= 0x9F) {
            // SETcc r/m8
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            bool cond = false;
            switch (op2 & 0x0F) {
                case 0x4: cond = (cpu->eflags & FLAG_ZF) != 0; break; // sete
                case 0x5: cond = (cpu->eflags & FLAG_ZF) == 0; break; // setne
                case 0x2: cond = (cpu->eflags & FLAG_CF) != 0; break; // setb
                case 0x3: cond = (cpu->eflags & FLAG_CF) == 0; break; // setae
                case 0xC: cond = ((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0); break; // setl
                case 0xD: cond = ((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0); break; // setge
                case 0xE: cond = ((cpu->eflags & FLAG_ZF) != 0) || (((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0)); break; // setle
                case 0xF: cond = ((cpu->eflags & FLAG_ZF) == 0) && (((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0)); break; // setg
                default: break;
            }
            uint8_t val = cond ? 1 : 0;
            if (m.is_reg) {
                if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | val;
                else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (val << 8);
            } else {
                mem_write_u8(m.eff_addr, val);
            }
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 >= 0x80 && op2 <= 0x8F) {
            // jcc rel32
            int32_t rel = (int32_t)mem_read_u32(pc);
            pc += 4;
            bool cond = false;
            switch (op2 & 0x0F) {
                case 0x4: cond = (cpu->eflags & FLAG_ZF) != 0; break; // jz / je
                case 0x5: cond = (cpu->eflags & FLAG_ZF) == 0; break; // jnz / jne
                case 0x2: cond = (cpu->eflags & FLAG_CF) != 0; break; // jb / jc
                case 0x3: cond = (cpu->eflags & FLAG_CF) == 0; break; // jae / jnc
                case 0x6: cond = ((cpu->eflags & FLAG_CF) != 0) || ((cpu->eflags & FLAG_ZF) != 0); break; // jbe / jna
                case 0x7: cond = ((cpu->eflags & FLAG_CF) == 0) && ((cpu->eflags & FLAG_ZF) == 0); break; // ja / jnbe
                case 0x8: cond = (cpu->eflags & FLAG_SF) != 0; break; // js
                case 0x9: cond = (cpu->eflags & FLAG_SF) == 0; break; // jns
                case 0xC: cond = ((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0); break; // jl
                case 0xD: cond = ((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0); break; // jge
                case 0xE: cond = ((cpu->eflags & FLAG_ZF) != 0) || (((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0)); break; // jle
                case 0xF: cond = ((cpu->eflags & FLAG_ZF) == 0) && (((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0)); break; // jg
                default: break;
            }
            if (cond) pc += rel;
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xB6 || op2 == 0xB7) {
            // movzx r32, r/m8 / r/m16
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t val = (op2 == 0xB6) ? (m.is_reg ? (cpu->regs[m.rm] & 0xFF) : mem_read_u8(m.eff_addr))
                                          : (m.is_reg ? (cpu->regs[m.rm] & 0xFFFF) : mem_read_u16(m.eff_addr));
            cpu->regs[m.reg] = val;
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xBE || op2 == 0xBF) {
            // movsx r32, r/m8 / r/m16
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t val = (op2 == 0xBE) ? (m.is_reg ? (uint32_t)(int32_t)(int8_t)(cpu->regs[m.rm] & 0xFF) : (uint32_t)(int32_t)(int8_t)mem_read_u8(m.eff_addr))
                                          : (m.is_reg ? (uint32_t)(int32_t)(int16_t)(cpu->regs[m.rm] & 0xFFFF) : (uint32_t)(int32_t)(int16_t)mem_read_u16(m.eff_addr));
            cpu->regs[m.reg] = val;
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 >= 0x40 && op2 <= 0x4F) {
            // cmovcc r32, r/m32
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t val = read_rm32(cpu, &m);
            bool cond = false;
            switch (op2 & 0x0F) {
                case 0x0: cond = (cpu->eflags & FLAG_OF) != 0; break;
                case 0x1: cond = (cpu->eflags & FLAG_OF) == 0; break;
                case 0x2: cond = (cpu->eflags & FLAG_CF) != 0; break;
                case 0x3: cond = (cpu->eflags & FLAG_CF) == 0; break;
                case 0x4: cond = (cpu->eflags & FLAG_ZF) != 0; break;
                case 0x5: cond = (cpu->eflags & FLAG_ZF) == 0; break;
                case 0x6: cond = ((cpu->eflags & FLAG_CF) != 0) || ((cpu->eflags & FLAG_ZF) != 0); break;
                case 0x7: cond = ((cpu->eflags & FLAG_CF) == 0) && ((cpu->eflags & FLAG_ZF) == 0); break;
                case 0x8: cond = (cpu->eflags & FLAG_SF) != 0; break;
                case 0x9: cond = (cpu->eflags & FLAG_SF) == 0; break;
                case 0xC: cond = ((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0); break;
                case 0xD: cond = ((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0); break;
                case 0xE: cond = ((cpu->eflags & FLAG_ZF) != 0) || (((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0)); break;
                case 0xF: cond = ((cpu->eflags & FLAG_ZF) == 0) && (((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0)); break;
                default: break;
            }
            if (cond) cpu->regs[m.reg] = val;
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xA3) {
            // bt r/m32, r32
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t val = read_rm32(cpu, &m);
            uint32_t bit = cpu->regs[m.reg] & 31;
            if (val & (1U << bit)) cpu->eflags |= FLAG_CF;
            else cpu->eflags &= ~FLAG_CF;
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xBA) {
            // Group 8 (bt/bts/btr/btc r/m32, imm8)
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t bit = mem_read_u8(pc++) & 31;
            uint32_t val = read_rm32(cpu, &m);
            if (val & (1U << bit)) cpu->eflags |= FLAG_CF;
            else cpu->eflags &= ~FLAG_CF;
            if (m.reg == 5) write_rm32(cpu, &m, val | (1U << bit));       // BTS
            else if (m.reg == 6) write_rm32(cpu, &m, val & ~(1U << bit));  // BTR
            else if (m.reg == 7) write_rm32(cpu, &m, val ^ (1U << bit));   // BTC
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xBC) {
            // bsf r32, r/m32
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t src = read_rm32(cpu, &m);
            if (src == 0) {
                cpu->eflags |= FLAG_ZF;
            } else {
                cpu->eflags &= ~FLAG_ZF;
                cpu->regs[m.reg] = (uint32_t)__builtin_ctz(src);
            }
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xBD) {
            // bsr r32, r/m32
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t src = read_rm32(cpu, &m);
            if (src == 0) {
                cpu->eflags |= FLAG_ZF;
            } else {
                cpu->eflags &= ~FLAG_ZF;
                cpu->regs[m.reg] = (uint32_t)(31 - __builtin_clz(src));
            }
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xAF) {
            // imul r32, r/m32
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t src = read_rm32(cpu, &m);
            int64_t res = (int64_t)(int32_t)cpu->regs[m.reg] * (int64_t)(int32_t)src;
            cpu->regs[m.reg] = (uint32_t)res;
            set_flags_logic(cpu, (uint32_t)res);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0x1F) {
            // NOP r/m32 (multi-byte NOP)
            decode_modrm(cpu, &pc, seg_base);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 >= 0x60 && op2 <= 0x7F) {
            // MMX / SSE / SSE2 packed data operations (punpck, movd, movq, etc.)
            decode_modrm(cpu, &pc, seg_base);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0x57 ||
                   op2 == 0xD6 || op2 == 0xE6 || op2 == 0x70) {
            // SSE MOV / shuffle / XOR instructions
            decode_modrm(cpu, &pc, seg_base);
            if (op2 == 0x70) pc++; // pshufw / pshufd imm8
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0x31) {
            // RDTSC - Read Time-Stamp Counter
            uint64_t tsc = (uint64_t)cpu->instruction_count * 10;
            cpu->eax = (uint32_t)(tsc & 0xFFFFFFFF);
            cpu->edx = (uint32_t)(tsc >> 32);
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        } else if (op2 == 0xA2) {
            // CPUID
            if (cpu->eax == 0) {
                cpu->eax = 1; // max leaf
                cpu->ebx = 0x756e6547; // "Genu"
                cpu->edx = 0x49656e69; // "ineI"
                cpu->ecx = 0x6c65746e; // "ntel"
            } else if (cpu->eax == 1) {
                cpu->eax = 0x000206A7; // Family 6 Model 42 Stepping 7
                cpu->ebx = 0x00000800;
                cpu->ecx = 0x0298E3FF; // SSE3, SSSE3, SSE4.1, SSE4.2, etc.
                cpu->edx = 0x078BFBFF; // FPU, TSC, MMX, FXSR, SSE, SSE2, etc.
            } else {
                cpu->eax = 0;
                cpu->ebx = 0;
                cpu->ecx = 0;
                cpu->edx = 0;
            }
            cpu->eip = pc;
            cpu->instruction_count++;
            return 1;
        }
    }

    // Standard x86 instructions
    switch (op) {
        // NOP
        case 0x90:
            break;

        // INC reg
        case 0x40 ... 0x47: {
            uint8_t reg = op - 0x40;
            uint32_t val = cpu->regs[reg];
            cpu->regs[reg] = val + 1;
            set_flags_add(cpu, val, 1, val + 1);
            break;
        }

        // DEC reg
        case 0x48 ... 0x4F: {
            uint8_t reg = op - 0x48;
            uint32_t val = cpu->regs[reg];
            cpu->regs[reg] = val - 1;
            set_flags_sub(cpu, val, 1, val - 1);
            break;
        }

        // XCHG EAX, reg
        case 0x91 ... 0x97: {
            uint8_t reg = op - 0x90;
            uint32_t tmp = cpu->eax;
            cpu->eax = cpu->regs[reg];
            cpu->regs[reg] = tmp;
            break;
        }

        // XCHG r/m32, r32
        case 0x87: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            write_rm32(cpu, &m, src);
            cpu->regs[m.reg] = dst;
            break;
        }

        // CDQ (Convert Doubleword to Quadword)
        case 0x99: {
            cpu->edx = (cpu->eax & 0x80000000) ? 0xFFFFFFFF : 0;
            break;
        }

        // PUSHFD / POPFD
        case 0x9C: // PUSHFD
            push32(cpu, cpu->eflags);
            break;
        case 0x9D: // POPFD
            cpu->eflags = pop32(cpu);
            break;

        // SAHF / LAHF
        case 0x9E: // SAHF: flags = AH
            cpu->eflags = (cpu->eflags & ~0xFF) | ((cpu->eax >> 8) & 0xFF);
            break;
        case 0x9F: // LAHF: AH = flags
            cpu->eax = (cpu->eax & ~0xFF00) | ((cpu->eflags & 0xFF) << 8);
            break;

        // STOSB (REP STOSB)
        case 0xAA: {
            if (has_rep) {
                while (cpu->ecx > 0) {
                    mem_write_u8(cpu->edi++, (uint8_t)(cpu->eax & 0xFF));
                    cpu->ecx--;
                }
            } else {
                mem_write_u8(cpu->edi++, (uint8_t)(cpu->eax & 0xFF));
            }
            break;
        }

        // STOSD (REP STOSD)
        case 0xAB: {
            if (has_rep) {
                while (cpu->ecx > 0) {
                    mem_write_u32(cpu->edi, cpu->eax);
                    cpu->edi += 4;
                    cpu->ecx--;
                }
            } else {
                mem_write_u32(cpu->edi, cpu->eax);
                cpu->edi += 4;
            }
            break;
        }

        // MOVSB (REP MOVSB)
        case 0xA4: {
            if (has_rep) {
                while (cpu->ecx > 0) {
                    mem_write_u8(cpu->edi++, mem_read_u8(cpu->esi++));
                    cpu->ecx--;
                }
            } else {
                mem_write_u8(cpu->edi++, mem_read_u8(cpu->esi++));
            }
            break;
        }

        // MOVSD (REP MOVSD)
        case 0xA5: {
            if (has_rep) {
                while (cpu->ecx > 0) {
                    mem_write_u32(cpu->edi, mem_read_u32(cpu->esi));
                    cpu->edi += 4;
                    cpu->esi += 4;
                    cpu->ecx--;
                }
            } else {
                mem_write_u32(cpu->edi, mem_read_u32(cpu->esi));
                cpu->edi += 4;
                cpu->esi += 4;
            }
            break;
        }

        // Shifts/Rotates by imm8 (0xC1)
        case 0xC1: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t count = mem_read_u8(pc++) & 0x1F;
            uint32_t val = read_rm32(cpu, &m);
            uint32_t res = val;
            switch (m.reg) {
                case 4: res = val << count; break; // SHL / SAL
                case 5: res = val >> count; break; // SHR
                case 7: res = (uint32_t)(((int32_t)val) >> count); break; // SAR
                default: break;
            }
            write_rm32(cpu, &m, res);
            set_flags_logic(cpu, res);
            break;
        }

        // Shifts/Rotates by 1 (0xD0/0xD1) or CL (0xD2/0xD3)
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t count = (op == 0xD0 || op == 0xD1) ? 1 : (cpu->ecx & 0x1F);
            uint32_t val = read_rm32(cpu, &m);
            uint32_t res = val;
            switch (m.reg) {
                case 0: res = (val << count) | (val >> (32 - count)); break; // ROL
                case 1: res = (val >> count) | (val << (32 - count)); break; // ROR
                case 4: res = val << count; break;                            // SHL / SAL
                case 5: res = val >> count; break;                            // SHR
                case 7: res = (uint32_t)(((int32_t)val) >> count); break;     // SAR
                default: break;
            }
            write_rm32(cpu, &m, res);
            set_flags_logic(cpu, res);
            break;
        }

        // PUSH reg
        case 0x50 ... 0x57:
            push32(cpu, cpu->regs[op - 0x50]);
            break;

        // POP reg
        case 0x58 ... 0x5F:
            cpu->regs[op - 0x58] = pop32(cpu);
            break;

        // PUSHAD
        case 0x60: {
            uint32_t orig_esp = cpu->esp;
            push32(cpu, cpu->eax);
            push32(cpu, cpu->ecx);
            push32(cpu, cpu->edx);
            push32(cpu, cpu->ebx);
            push32(cpu, orig_esp);
            push32(cpu, cpu->ebp);
            push32(cpu, cpu->esi);
            push32(cpu, cpu->edi);
            break;
        }

        // POPAD
        case 0x61: {
            cpu->edi = pop32(cpu);
            cpu->esi = pop32(cpu);
            cpu->ebp = pop32(cpu);
            pop32(cpu); // discard original esp
            cpu->ebx = pop32(cpu);
            cpu->edx = pop32(cpu);
            cpu->ecx = pop32(cpu);
            cpu->eax = pop32(cpu);
            break;
        }

        // PUSH imm32
        case 0x68: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            push32(cpu, imm);
            break;
        }

        // IMUL r32, r/m32, imm32
        case 0x69: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t src = read_rm32(cpu, &m);
            int32_t imm = (int32_t)mem_read_u32(pc);
            pc += 4;
            int64_t res = (int64_t)(int32_t)src * (int64_t)imm;
            cpu->regs[m.reg] = (uint32_t)res;
            set_flags_logic(cpu, (uint32_t)res);
            break;
        }

        // PUSH imm8
        case 0x6A: {
            int8_t imm8 = (int8_t)mem_read_u8(pc++);
            push32(cpu, (int32_t)imm8);
            break;
        }

        // IMUL r32, r/m32, imm8
        case 0x6B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t src = read_rm32(cpu, &m);
            int32_t imm = (int32_t)(int8_t)mem_read_u8(pc++);
            int64_t res = (int64_t)(int32_t)src * (int64_t)imm;
            cpu->regs[m.reg] = (uint32_t)res;
            set_flags_logic(cpu, (uint32_t)res);
            break;
        }

        // MOV AL, moffset8
        case 0xA0: {
            uint32_t addr = mem_read_u32(pc);
            pc += 4;
            cpu->eax = (cpu->eax & ~0xFF) | mem_read_u8(seg_base + addr);
            break;
        }

        // MOV EAX, moffset32
        case 0xA1: {
            uint32_t addr = mem_read_u32(pc);
            pc += 4;
            cpu->eax = mem_read_u32(seg_base + addr);
            break;
        }

        // MOV moffset8, AL
        case 0xA2: {
            uint32_t addr = mem_read_u32(pc);
            pc += 4;
            mem_write_u8(seg_base + addr, cpu->eax & 0xFF);
            break;
        }

        // MOV moffset32, EAX
        case 0xA3: {
            uint32_t addr = mem_read_u32(pc);
            pc += 4;
            mem_write_u32(seg_base + addr, cpu->eax);
            break;
        }

        // SBB r/m32, r32
        case 0x19: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t cf = (cpu->eflags & FLAG_CF) ? 1 : 0;
            uint32_t res = dst - src - cf;
            write_rm32(cpu, &m, res);
            set_flags_sub(cpu, dst, src + cf, res);
            break;
        }

        // SBB r32, r/m32
        case 0x1B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t cf = (cpu->eflags & FLAG_CF) ? 1 : 0;
            uint32_t res = dst - src - cf;
            cpu->regs[m.reg] = res;
            set_flags_sub(cpu, dst, src + cf, res);
            break;
        }

        // OR r/m32, r32
        case 0x09: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst | src;
            write_rm32(cpu, &m, res);
            set_flags_logic(cpu, res);
            break;
        }

        // OR r32, r/m32
        case 0x0B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t res = dst | src;
            cpu->regs[m.reg] = res;
            set_flags_logic(cpu, res);
            break;
        }

        // ADD EAX, imm32
        case 0x05: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            uint32_t res = cpu->eax + imm;
            set_flags_add(cpu, cpu->eax, imm, res);
            cpu->eax = res;
            break;
        }

        // OR EAX, imm32
        case 0x0D: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            cpu->eax |= imm;
            set_flags_logic(cpu, cpu->eax);
            break;
        }

        // AND EAX, imm32
        case 0x25: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            cpu->eax &= imm;
            set_flags_logic(cpu, cpu->eax);
            break;
        }

        // SUB EAX, imm32
        case 0x2D: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            uint32_t res = cpu->eax - imm;
            set_flags_sub(cpu, cpu->eax, imm, res);
            cpu->eax = res;
            break;
        }

        // XOR EAX, imm32
        case 0x35: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            cpu->eax ^= imm;
            set_flags_logic(cpu, cpu->eax);
            break;
        }

        // ADD AL, imm8
        case 0x04: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t al = (uint8_t)(cpu->eax & 0xFF);
            uint32_t res = al + imm8;
            cpu->eax = (cpu->eax & 0xFFFFFF00) | (res & 0xFF);
            set_flags_add(cpu, al, imm8, (uint8_t)res);
            break;
        }

        // OR AL, imm8
        case 0x0C: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t res = (cpu->eax & 0xFF) | imm8;
            cpu->eax = (cpu->eax & 0xFFFFFF00) | res;
            set_flags_logic(cpu, res);
            break;
        }

        // AND AL, imm8
        case 0x24: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t res = (cpu->eax & 0xFF) & imm8;
            cpu->eax = (cpu->eax & 0xFFFFFF00) | res;
            set_flags_logic(cpu, res);
            break;
        }

        // SUB AL, imm8
        case 0x2C: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t al = (uint8_t)(cpu->eax & 0xFF);
            uint8_t res = al - imm8;
            cpu->eax = (cpu->eax & 0xFFFFFF00) | res;
            set_flags_sub(cpu, al, imm8, res);
            break;
        }

        // XOR AL, imm8
        case 0x34: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t res = (cpu->eax & 0xFF) ^ imm8;
            cpu->eax = (cpu->eax & 0xFFFFFF00) | res;
            set_flags_logic(cpu, res);
            break;
        }

        // CMP AL, imm8
        case 0x3C: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t al = (uint8_t)(cpu->eax & 0xFF);
            uint8_t res = al - imm8;
            set_flags_sub(cpu, al, imm8, res);
            break;
        }

        // CMP EAX, imm32
        case 0x3D: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            set_flags_sub(cpu, cpu->eax, imm, cpu->eax - imm);
            break;
        }

        // TEST AL, imm8
        case 0xA8: {
            uint8_t imm8 = mem_read_u8(pc++);
            set_flags_logic(cpu, (cpu->eax & 0xFF) & imm8);
            break;
        }

        // TEST EAX, imm32
        case 0xA9: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            set_flags_logic(cpu, cpu->eax & imm);
            break;
        }

        // MOV r/m8, r8
        case 0x88: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t val8 = (m.reg < 4) ? (cpu->regs[m.reg] & 0xFF) : ((cpu->regs[m.reg - 4] >> 8) & 0xFF);
            if (m.is_reg) {
                if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | val8;
                else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (val8 << 8);
            } else {
                mem_write_u8(m.eff_addr, val8);
            }
            break;
        }

        // MOV r8, r/m8
        case 0x8A: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t val8 = 0;
            if (m.is_reg) {
                val8 = (m.rm < 4) ? (cpu->regs[m.rm] & 0xFF) : ((cpu->regs[m.rm - 4] >> 8) & 0xFF);
            } else {
                val8 = mem_read_u8(m.eff_addr);
            }
            if (m.reg < 4) cpu->regs[m.reg] = (cpu->regs[m.reg] & ~0xFF) | val8;
            else cpu->regs[m.reg - 4] = (cpu->regs[m.reg - 4] & ~0xFF00) | (val8 << 8);
            break;
        }

        // MOV r8, imm8
        case 0xB0 ... 0xB7: {
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t reg8 = op - 0xB0;
            if (reg8 < 4) {
                cpu->regs[reg8] = (cpu->regs[reg8] & ~0xFF) | imm8;
            } else {
                cpu->regs[reg8 - 4] = (cpu->regs[reg8 - 4] & ~0xFF00) | ((uint32_t)imm8 << 8);
            }
            break;
        }

        // MOV reg, imm32
        case 0xB8 ... 0xBF: {
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            cpu->regs[op - 0xB8] = imm;
            break;
        }

        // MOV r/m32, r32
        case 0x89: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            write_rm32(cpu, &m, cpu->regs[m.reg]);
            break;
        }

        // MOV r32, r/m32
        case 0x8B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            cpu->regs[m.reg] = read_rm32(cpu, &m);
            break;
        }

        // MOV r/m8, imm8
        case 0xC6: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t imm8 = mem_read_u8(pc++);
            if (m.is_reg) {
                if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | imm8;
                else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (imm8 << 8);
            } else {
                mem_write_u8(m.eff_addr, imm8);
            }
            break;
        }

        // MOV r/m32, imm32
        case 0xC7: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t imm = mem_read_u32(pc);
            pc += 4;
            write_rm32(cpu, &m, imm);
            break;
        }

        // LEA r32, m
        case 0x8D: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            cpu->regs[m.reg] = m.eff_addr - seg_base;
            break;
        }

        // ADD r/m32, r32
        case 0x01: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst + src;
            write_rm32(cpu, &m, res);
            set_flags_add(cpu, dst, src, res);
            break;
        }

        // ADD r32, r/m32
        case 0x03: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t res = dst + src;
            cpu->regs[m.reg] = res;
            set_flags_add(cpu, dst, src, res);
            break;
        }

        // SUB r/m32, r32
        case 0x29: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst - src;
            write_rm32(cpu, &m, res);
            set_flags_sub(cpu, dst, src, res);
            break;
        }

        // SUB r32, r/m32
        case 0x2B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t res = dst - src;
            cpu->regs[m.reg] = res;
            set_flags_sub(cpu, dst, src, res);
            break;
        }

        // CMP r/m32, r32
        case 0x39: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            set_flags_sub(cpu, dst, src, dst - src);
            break;
        }

        // CMP r32, r/m32
        case 0x3B: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            set_flags_sub(cpu, dst, src, dst - src);
            break;
        }

        // XOR r/m32, r32
        case 0x31: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst ^ src;
            write_rm32(cpu, &m, res);
            set_flags_logic(cpu, res);
            break;
        }

        // XOR r32, r/m32
        case 0x33: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t res = dst ^ src;
            cpu->regs[m.reg] = res;
            set_flags_logic(cpu, res);
            break;
        }

        // AND r/m32, r32
        case 0x21: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            uint32_t res = dst & src;
            write_rm32(cpu, &m, res);
            set_flags_logic(cpu, res);
            break;
        }

        // AND r32, r/m32
        case 0x23: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = cpu->regs[m.reg];
            uint32_t src = read_rm32(cpu, &m);
            uint32_t res = dst & src;
            cpu->regs[m.reg] = res;
            set_flags_logic(cpu, res);
            break;
        }

        // TEST r/m32, r32
        case 0x85: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t dst = read_rm32(cpu, &m);
            uint32_t src = cpu->regs[m.reg];
            set_flags_logic(cpu, dst & src);
            break;
        }

        // ADD r/m8, r8
        case 0x00: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_rm8(cpu, &m);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t res = dst + src;
            write_rm8(cpu, &m, res);
            set_flags_add8(cpu, dst, src, res);
            break;
        }

        // ADD r8, r/m8
        case 0x02: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            uint8_t res = dst + src;
            write_r8(cpu, m.reg, res);
            set_flags_add8(cpu, dst, src, res);
            break;
        }

        // OR r/m8, r8
        case 0x08: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_rm8(cpu, &m);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t res = dst | src;
            write_rm8(cpu, &m, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // OR r8, r/m8
        case 0x0A: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            uint8_t res = dst | src;
            write_r8(cpu, m.reg, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // AND r/m8, r8
        case 0x20: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_rm8(cpu, &m);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t res = dst & src;
            write_rm8(cpu, &m, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // AND r8, r/m8
        case 0x22: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            uint8_t res = dst & src;
            write_r8(cpu, m.reg, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // SUB r/m8, r8
        case 0x28: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_rm8(cpu, &m);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t res = dst - src;
            write_rm8(cpu, &m, res);
            set_flags_sub8(cpu, dst, src, res);
            break;
        }

        // SUB r8, r/m8
        case 0x2A: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            uint8_t res = dst - src;
            write_r8(cpu, m.reg, res);
            set_flags_sub8(cpu, dst, src, res);
            break;
        }

        // XOR r/m8, r8
        case 0x30: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_rm8(cpu, &m);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t res = dst ^ src;
            write_rm8(cpu, &m, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // XOR r8, r/m8
        case 0x32: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            uint8_t res = dst ^ src;
            write_r8(cpu, m.reg, res);
            set_flags_logic8(cpu, res);
            break;
        }

        // TEST r/m8, r8
        case 0x84: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t dst = read_rm8(cpu, &m);
            set_flags_logic8(cpu, dst & src);
            break;
        }

        // CMP r/m8, r8
        case 0x38: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t src = read_r8(cpu, m.reg);
            uint8_t dst = read_rm8(cpu, &m);
            set_flags_sub8(cpu, dst, src, dst - src);
            break;
        }

        // CMP r8, r/m8
        case 0x3A: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t dst = read_r8(cpu, m.reg);
            uint8_t src = read_rm8(cpu, &m);
            set_flags_sub8(cpu, dst, src, dst - src);
            break;
        }

        // Group 3 (0xF7: TEST, NOT, NEG, MUL, IMUL, DIV, IDIV r/m32)
        // Group 3 8-bit (TEST, NOT, NEG, MUL, IMUL, DIV, IDIV r/m8)
        case 0xF6: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t val8 = 0;
            if (m.is_reg) {
                if (m.rm < 4) val8 = cpu->regs[m.rm] & 0xFF;
                else val8 = (cpu->regs[m.rm - 4] >> 8) & 0xFF;
            } else {
                val8 = mem_read_u8(m.eff_addr);
            }

            switch (m.reg) {
                case 0: { // TEST r/m8, imm8
                    uint8_t imm8 = mem_read_u8(pc++);
                    set_flags_logic(cpu, val8 & imm8);
                    break;
                }
                case 2: { // NOT r/m8
                    uint8_t res = ~val8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    break;
                }
                case 3: { // NEG r/m8
                    uint8_t res = 0 - val8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_sub(cpu, 0, val8, res);
                    break;
                }
                case 4: { // MUL AL, r/m8
                    uint16_t prod = (uint16_t)(cpu->eax & 0xFF) * (uint16_t)val8;
                    cpu->eax = (cpu->eax & ~0xFFFF) | prod;
                    if (prod > 0xFF) cpu->eflags |= (FLAG_CF | FLAG_OF);
                    else cpu->eflags &= ~(FLAG_CF | FLAG_OF);
                    break;
                }
                case 5: { // IMUL AL, r/m8
                    int16_t prod = (int16_t)(int8_t)(cpu->eax & 0xFF) * (int16_t)(int8_t)val8;
                    cpu->eax = (cpu->eax & ~0xFFFF) | (uint16_t)prod;
                    break;
                }
                case 6: { // DIV r/m8
                    if (val8 != 0) {
                        uint16_t ax = (uint16_t)(cpu->eax & 0xFFFF);
                        uint8_t q = (uint8_t)(ax / val8);
                        uint8_t r = (uint8_t)(ax % val8);
                        cpu->eax = (cpu->eax & ~0xFFFF) | ((uint16_t)r << 8) | q;
                    }
                    break;
                }
                case 7: { // IDIV r/m8
                    if (val8 != 0) {
                        int16_t ax = (int16_t)(cpu->eax & 0xFFFF);
                        int8_t div = (int8_t)val8;
                        int8_t q = (int8_t)(ax / div);
                        int8_t r = (int8_t)(ax % div);
                        cpu->eax = (cpu->eax & ~0xFFFF) | (((uint16_t)(uint8_t)r) << 8) | (uint8_t)q;
                    }
                    break;
                }
                default:
                    fprintf(stderr, "CPU: Unhandled Group 3 8-bit sub-op %u\n", m.reg);
                    break;
            }
            break;
        }

        case 0xF7: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t val = read_rm32(cpu, &m);
            switch (m.reg) {
                case 0: { // TEST r/m32, imm32
                    uint32_t imm = mem_read_u32(pc);
                    pc += 4;
                    set_flags_logic(cpu, val & imm);
                    break;
                }
                case 2: { // NOT r/m32
                    write_rm32(cpu, &m, ~val);
                    break;
                }
                case 3: { // NEG r/m32
                    uint32_t res = 0 - val;
                    write_rm32(cpu, &m, res);
                    set_flags_sub(cpu, 0, val, res);
                    break;
                }
                case 4: { // MUL EAX, r/m32 (unsigned)
                    uint64_t prod = (uint64_t)cpu->eax * (uint64_t)val;
                    cpu->eax = (uint32_t)prod;
                    cpu->edx = (uint32_t)(prod >> 32);
                    if (cpu->edx) cpu->eflags |= (FLAG_CF | FLAG_OF);
                    else cpu->eflags &= ~(FLAG_CF | FLAG_OF);
                    break;
                }
                case 5: { // IMUL EAX, r/m32 (signed)
                    int64_t prod = (int64_t)(int32_t)cpu->eax * (int64_t)(int32_t)val;
                    cpu->eax = (uint32_t)prod;
                    cpu->edx = (uint32_t)(prod >> 32);
                    break;
                }
                case 6: { // DIV r/m32
                    if (val != 0) {
                        uint64_t dividend = ((uint64_t)cpu->edx << 32) | cpu->eax;
                        cpu->eax = (uint32_t)(dividend / val);
                        cpu->edx = (uint32_t)(dividend % val);
                    }
                    break;
                }
                case 7: { // IDIV r/m32
                    if (val != 0) {
                        int64_t dividend = (int64_t)(((uint64_t)cpu->edx << 32) | cpu->eax);
                        int32_t divisor = (int32_t)val;
                        cpu->eax = (uint32_t)(dividend / divisor);
                        cpu->edx = (uint32_t)(dividend % divisor);
                    }
                    break;
                }
                default:
                    fprintf(stderr, "CPU: Unhandled Group 3 sub-op %u\n", m.reg);
                    break;
            }
            break;
        }

        // Group 1 Byte (0x80: ADD, OR, AND, SUB, CMP r/m8, imm8)
        case 0x80: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint8_t imm8 = mem_read_u8(pc++);
            uint8_t val8 = 0;
            if (m.is_reg) {
                // Low 8-bit or High 8-bit register
                if (m.rm < 4) val8 = cpu->regs[m.rm] & 0xFF;
                else val8 = (cpu->regs[m.rm - 4] >> 8) & 0xFF;
            } else {
                val8 = mem_read_u8(m.eff_addr);
            }

            switch (m.reg) {
                case 0: { // ADD
                    uint8_t res = val8 + imm8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_add(cpu, val8, imm8, res);
                    break;
                }
                case 1: { // OR
                    uint8_t res = val8 | imm8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 4: { // AND
                    uint8_t res = val8 & imm8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 5: { // SUB
                    uint8_t res = val8 - imm8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_sub(cpu, val8, imm8, res);
                    break;
                }
                case 6: { // XOR
                    uint8_t res = val8 ^ imm8;
                    if (m.is_reg) {
                        if (m.rm < 4) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFF) | res;
                        else cpu->regs[m.rm - 4] = (cpu->regs[m.rm - 4] & ~0xFF00) | (res << 8);
                    } else mem_write_u8(m.eff_addr, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 7: { // CMP
                    set_flags_sub(cpu, val8, imm8, (uint32_t)(uint8_t)(val8 - imm8));
                    break;
                }
                default:
                    fprintf(stderr, "CPU: Unhandled opcode 0x80 sub-op %u\n", m.reg);
                    break;
            }
            break;
        }

        // ADD/OR/AND/SUB/CMP r/m, imm8 (0x83) or imm16/32 (0x81)
        case 0x81:
        case 0x83: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t imm = 0;
            if (op == 0x83) {
                imm = (uint32_t)(int32_t)(int8_t)mem_read_u8(pc++);
            } else if (is_op16) {
                imm = mem_read_u16(pc);
                pc += 2;
            } else {
                imm = mem_read_u32(pc);
                pc += 4;
            }
            uint32_t val = is_op16 ? (m.is_reg ? (cpu->regs[m.rm] & 0xFFFF) : mem_read_u16(m.eff_addr))
                                   : read_rm32(cpu, &m);

            switch (m.reg) {
                case 0: { // ADD
                    uint32_t res = val + imm;
                    if (is_op16) {
                        if (m.is_reg) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFFFF) | (res & 0xFFFF);
                        else mem_write_u16(m.eff_addr, (uint16_t)res);
                    } else write_rm32(cpu, &m, res);
                    set_flags_add(cpu, val, imm, res);
                    break;
                }
                case 1: { // OR
                    uint32_t res = val | imm;
                    if (is_op16) {
                        if (m.is_reg) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFFFF) | (res & 0xFFFF);
                        else mem_write_u16(m.eff_addr, (uint16_t)res);
                    } else write_rm32(cpu, &m, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 4: { // AND
                    uint32_t res = val & imm;
                    if (is_op16) {
                        if (m.is_reg) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFFFF) | (res & 0xFFFF);
                        else mem_write_u16(m.eff_addr, (uint16_t)res);
                    } else write_rm32(cpu, &m, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 5: { // SUB
                    uint32_t res = val - imm;
                    if (is_op16) {
                        if (m.is_reg) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFFFF) | (res & 0xFFFF);
                        else mem_write_u16(m.eff_addr, (uint16_t)res);
                    } else write_rm32(cpu, &m, res);
                    set_flags_sub(cpu, val, imm, res);
                    break;
                }
                case 6: { // XOR
                    uint32_t res = val ^ imm;
                    if (is_op16) {
                        if (m.is_reg) cpu->regs[m.rm] = (cpu->regs[m.rm] & ~0xFFFF) | (res & 0xFFFF);
                        else mem_write_u16(m.eff_addr, (uint16_t)res);
                    } else write_rm32(cpu, &m, res);
                    set_flags_logic(cpu, res);
                    break;
                }
                case 7: { // CMP
                    set_flags_sub(cpu, val, imm, val - imm);
                    break;
                }
                default:
                    fprintf(stderr, "CPU: Unhandled Group 1 sub-op %u\n", m.reg);
                    break;
            }
            break;
        }

        // CALL rel32
        case 0xE8: {
            int32_t rel = (int32_t)mem_read_u32(pc);
            pc += 4;
            push32(cpu, pc);
            pc += rel;
            break;
        }

        // JMP rel32
        case 0xE9: {
            int32_t rel = (int32_t)mem_read_u32(pc);
            pc += 4;
            pc += rel;
            break;
        }

        // JMP rel8
        case 0xEB: {
            int8_t rel = (int8_t)mem_read_u8(pc++);
            pc += rel;
            break;
        }

        // Jcc rel8
        case 0x70 ... 0x7F: {
            int8_t rel = (int8_t)mem_read_u8(pc++);
            bool cond = false;
            switch (op & 0x0F) {
                case 0x4: cond = (cpu->eflags & FLAG_ZF) != 0; break; // jz / je
                case 0x5: cond = (cpu->eflags & FLAG_ZF) == 0; break; // jnz / jne
                case 0x2: cond = (cpu->eflags & FLAG_CF) != 0; break; // jb / jc
                case 0x3: cond = (cpu->eflags & FLAG_CF) == 0; break; // jae / jnc
                case 0x6: cond = ((cpu->eflags & FLAG_CF) != 0) || ((cpu->eflags & FLAG_ZF) != 0); break; // jbe / jna
                case 0x7: cond = ((cpu->eflags & FLAG_CF) == 0) && ((cpu->eflags & FLAG_ZF) == 0); break; // ja / jnbe
                case 0x8: cond = (cpu->eflags & FLAG_SF) != 0; break; // js
                case 0x9: cond = (cpu->eflags & FLAG_SF) == 0; break; // jns
                case 0xC: cond = ((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0); break; // jl
                case 0xD: cond = ((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0); break; // jge
                case 0xE: cond = ((cpu->eflags & FLAG_ZF) != 0) || (((cpu->eflags & FLAG_SF) != 0) != ((cpu->eflags & FLAG_OF) != 0)); break; // jle
                case 0xF: cond = ((cpu->eflags & FLAG_ZF) == 0) && (((cpu->eflags & FLAG_SF) != 0) == ((cpu->eflags & FLAG_OF) != 0)); break; // jg
                default: break;
            }
            if (cond) pc += rel;
            break;
        }

        // RET
        case 0xC3: {
            pc = pop32(cpu);
            break;
        }

        // RET imm16
        case 0xC2: {
            uint16_t bytes = mem_read_u16(pc);
            pc = pop32(cpu);
            cpu->esp += bytes;
            break;
        }

        // LEAVE
        case 0xC9: {
            cpu->esp = cpu->ebp;
            cpu->ebp = pop32(cpu);
            break;
        }

        // x87 FPU Instructions (0xD8 ... 0xDF: fninit, fld, fstp, etc.)
        case 0xD8 ... 0xDF: {
            uint8_t sub = mem_read_u8(pc++);
            if (sub < 0xC0) {
                // ModRM memory operand: decode and advance pc
                pc--;
                decode_modrm(cpu, &pc, seg_base);
            }
            break;
        }

        // Group 5 (0xFF: INC, DEC, CALL, JMP, PUSH)
        case 0xFF: {
            ModRM m = decode_modrm(cpu, &pc, seg_base);
            uint32_t target = read_rm32(cpu, &m);
            switch (m.reg) {
                case 0: { // INC r/m32
                    uint32_t res = target + 1;
                    write_rm32(cpu, &m, res);
                    set_flags_add(cpu, target, 1, res);
                    break;
                }
                case 1: { // DEC r/m32
                    uint32_t res = target - 1;
                    write_rm32(cpu, &m, res);
                    set_flags_sub(cpu, target, 1, res);
                    break;
                }
                case 2: // CALL r/m32
                    push32(cpu, pc);
                    pc = target;
                    break;
                case 4: // JMP r/m32
                    pc = target;
                    break;
                case 6: // PUSH r/m32
                    push32(cpu, target);
                    break;
                default:
                    fprintf(stderr, "CPU: Unhandled Group 5 sub-op %u\n", m.reg);
                    break;
            }
            break;
        }

        default:
            fprintf(stderr, "CPU: Unhandled opcode 0x%02X at EIP=0x%08X\n", op, cpu->eip);
            cpu->halted = true;
            return -1;
    }

    cpu->eip = pc;
    cpu->instruction_count++;
    return 1;
}

int cpu_run(CPUState* cpu, uint64_t max_instructions) {
    while (!cpu->halted && (max_instructions == 0 || cpu->instruction_count < max_instructions)) {
        if (g_dynarec.enabled || g_dynarec.dump_jit) {
            TranslationBlock* tb = arm64_dynarec_find(cpu->eip);
            if (!tb) {
                tb = arm64_dynarec_compile(cpu, cpu->eip);
            }
            if (tb && tb->x86_size > 0 && g_dynarec.enabled) {
                arm64_dynarec_exec(cpu, tb);
                continue;
            }
        }
        if (cpu_step(cpu) <= 0) break;
    }
    return cpu->exit_code;
}
