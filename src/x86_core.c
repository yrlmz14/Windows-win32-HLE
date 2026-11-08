// x86_core.c — Complete 32-bit x86 Interpreter Core for Kindle Win32 HLE
#include "x86_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t mod;
    uint8_t reg;
    uint8_t rm;       /* effective base register (resolved through SIB if applicable) */
    int32_t disp;
    uint8_t len;      /* total length of modrm + SIB + displacement bytes */
    bool has_sib;
    uint8_t sib_base;
    uint8_t sib_index;
    uint8_t sib_scale; /* 1, 2, 4, or 8 */
} modrm_t;

/**
 * @brief Compute effective address from a decoded ModR/M+SIB.
 *
 * For mod != 3 (memory operands), this returns the linear address
 * using the GPR array for register values.
 */

static uint32_t modrm_effective_addr(const modrm_t *m, const uint32_t *gprs) {
    uint32_t addr = 0;

    if (m->has_sib) {
        /* SIB addressing: [base + index*scale + disp] */
        if (m->mod == 0 && m->sib_base == 5) {
            /* mod=0, base=5 → disp32 (no base register) */
            addr = (uint32_t)m->disp;
        } else {
            addr = gprs[m->sib_base];
        }
        if (m->sib_index != 4) { /* index=4 means "no index" */
            addr += gprs[m->sib_index] * m->sib_scale;
        }
        if (m->mod == 1 || m->mod == 2) {
            addr += (uint32_t)m->disp;
        }
    } else {
        /* Simple [reg + disp] addressing */
        if (m->mod == 0 && m->rm == 5) {
            addr = (uint32_t)m->disp; /* [disp32] absolute */
        } else {
            addr = gprs[m->rm] + (uint32_t)m->disp;
        }
    }
    return addr;
}

static bool decode_modrm(const uint8_t *code, modrm_t *out) {
    uint8_t b = code[0];
    out->mod = (b >> 6) & 3;
    out->reg = (b >> 3) & 7;
    out->rm  = b & 7;
    out->len = 1;
    out->disp = 0;
    out->has_sib = false;
    out->sib_base = 0;
    out->sib_index = 0;
    out->sib_scale = 1;

    /* Register-direct mode: no displacement, no SIB */
    if (out->mod == 3) return true;

    /* SIB byte present when rm == 4 */
    if (out->rm == 4) {
        uint8_t sib = code[out->len];
        out->len++;
        out->has_sib = true;
        out->sib_scale = 1 << ((sib >> 6) & 3);
        out->sib_index = (sib >> 3) & 7;
        out->sib_base  = sib & 7;

        if (out->mod == 0) {
            if (out->sib_base == 5) {
                /* mod=0, base=5: [disp32 + index*scale] */
                out->disp = (int32_t)((uint32_t)code[out->len] |
                            ((uint32_t)code[out->len+1] << 8) |
                            ((uint32_t)code[out->len+2] << 16) |
                            ((uint32_t)code[out->len+3] << 24));
                out->len += 4;
            }
            /* else: [base + index*scale], no displacement */
        } else if (out->mod == 1) {
            out->disp = (int8_t)code[out->len];
            out->len += 1;
        } else if (out->mod == 2) {
            out->disp = (int32_t)((uint32_t)code[out->len] |
                        ((uint32_t)code[out->len+1] << 8) |
                        ((uint32_t)code[out->len+2] << 16) |
                        ((uint32_t)code[out->len+3] << 24));
            out->len += 4;
        }
        /* Set rm to base register for simple cases where code reads m.rm */
        out->rm = out->sib_base;
    } else {
        /* No SIB */
        if (out->mod == 0) {
            if (out->rm == 5) {
                out->disp = (int32_t)((uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                                      ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24));
                out->len += 4;
            }
        } else if (out->mod == 1) {
            out->disp = (int8_t)code[out->len];
            out->len += 1;
        } else if (out->mod == 2) {
            out->disp = (int32_t)((uint32_t)code[out->len] |
                        ((uint32_t)code[out->len+1] << 8) |
                        ((uint32_t)code[out->len+2] << 16) |
                        ((uint32_t)code[out->len+3] << 24));
            out->len += 4;
        }
    }
    return true;
}

static inline uint8_t get_reg8(const uint32_t *gprs, uint8_t reg) {
    if (reg < 4) {
        return (uint8_t)(gprs[reg] & 0xFF);
    } else {
        return (uint8_t)((gprs[reg - 4] >> 8) & 0xFF);
    }
}

static inline void set_reg8(uint32_t *gprs, uint8_t reg, uint8_t val) {
    if (reg < 4) {
        gprs[reg] = (gprs[reg] & ~0xFFU) | (uint32_t)val;
    } else {
        gprs[reg - 4] = (gprs[reg - 4] & ~0xFF00U) | ((uint32_t)val << 8);
    }
}

static inline uint32_t fs_read32(const x86_context_t *ctx, uint32_t offset) {
    if (offset / 4 < 64) {
        return ctx->tib_data[offset / 4];
    }
    return 0;
}

static inline void fs_write32(x86_context_t *ctx, uint32_t offset, uint32_t val) {
    if (offset / 4 < 64) {
        ctx->tib_data[offset / 4] = val;
    }
}

static inline uint32_t exec_group1(x86_context_t *ctx, uint8_t reg, uint32_t val, uint32_t imm, bool is16) {
    uint32_t mask = is16 ? 0xFFFFU : 0xFFFFFFFFU;
    uint32_t sign_bit = is16 ? 0x8000U : 0x80000000U;
    uint32_t res = 0;
    val &= mask;
    imm &= mask;

    switch (reg) {
        case 0: /* ADD */
            res = (val + imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (res < val) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((val ^ ~imm) & (val ^ res)) & sign_bit) ctx->regs.eflags |= X86_EFLAGS_OF;
            return res;
        case 1: /* OR */
            res = (val | imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            return res;
        case 4: /* AND */
            res = (val & imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            return res;
        case 5: /* SUB */
            res = (val - imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((val ^ imm) & (val ^ res)) & sign_bit) ctx->regs.eflags |= X86_EFLAGS_OF;
            return res;
        case 6: /* XOR */
            res = (val ^ imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            return res;
        case 7: /* CMP */
            res = (val - imm) & mask;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & sign_bit) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((val ^ imm) & (val ^ res)) & sign_bit) ctx->regs.eflags |= X86_EFLAGS_OF;
            return val;
        default:
            return val;
    }
}

static uint32_t s_last_eips[32];
static size_t s_last_eip_idx = 0;

/**
 * @brief Fallback single-instruction interpreter.
 */

bool x86_interpret_instruction(x86_context_t *ctx) {
    if (!ctx) return false;

    if (ctx->regs.eip < 0x00010000) {
        fprintf(stderr, "[Interpreter] Wild jump/return caught! EIP=0x%08X (ESP=0x%08X)\n", (unsigned int)ctx->regs.eip, (unsigned int)ctx->regs.esp);
        for (int k = 0; k < 8; k++) {
            uint32_t val = 0;
            x86_mem_read32(ctx, ctx->regs.esp + k * 4, &val);
            fprintf(stderr, "  stack[%d] (ESP+%d) = 0x%08X\n", k, k * 4, (unsigned int)val);
        }
        ctx->running = false;
        return false;
    }

    uint8_t instr_buf[16];
    uint8_t *code;
    if ((ctx->regs.eip & 0xFFF) > 4096 - 16) {
        for (int b = 0; b < 16; b++) {
            uint8_t byte_val = 0;
            x86_mem_read8(ctx, ctx->regs.eip + b, &byte_val);
            instr_buf[b] = byte_val;
        }
        code = instr_buf;
    } else {
        code = x86_va_to_host(ctx, ctx->regs.eip);
        if (!code) {
            fprintf(stderr, "[Interpreter] Segmentation fault at EIP=0x%08X\n", (unsigned int)ctx->regs.eip);
            ctx->running = false;
            return false;
        }
    }

    uint8_t op = code[0];

    /* Helper: access GPR by index (eax=0, ecx=1, edx=2, ebx=3, esp=4, ebp=5, esi=6, edi=7) */
    uint32_t *gprs = (uint32_t *)&ctx->regs;

    /* ====== FS Segment Prefix (0x64) ====== */
    if (op == 0x64) {
        uint8_t op2 = code[1];

        /* MOV EAX, FS:[disp32] (0x64 0xA1) */
        if (op2 == 0xA1) {
            uint32_t disp32 = (uint32_t)code[2] | ((uint32_t)code[3] << 8) |
                              ((uint32_t)code[4] << 16) | ((uint32_t)code[5] << 24);
            ctx->regs.eax = fs_read32(ctx, disp32);
            ctx->regs.eip += 6;
            return true;
        }

        /* MOV FS:[disp32], EAX (0x64 0xA3) */
        if (op2 == 0xA3) {
            uint32_t disp32 = (uint32_t)code[2] | ((uint32_t)code[3] << 8) |
                              ((uint32_t)code[4] << 16) | ((uint32_t)code[5] << 24);
            fs_write32(ctx, disp32, ctx->regs.eax);
            ctx->regs.eip += 6;
            return true;
        }

        /* MOV FS:[m], r32 (0x64 0x89) */
        if (op2 == 0x89) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t addr = modrm_effective_addr(&m, gprs);
            fs_write32(ctx, addr, gprs[m.reg]);
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOV r32, FS:[m] (0x64 0x8B) */
        if (op2 == 0x8B) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t addr = modrm_effective_addr(&m, gprs);
            gprs[m.reg] = fs_read32(ctx, addr);
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* POP FS:[m] (0x64 0x8F) */
        if (op2 == 0x8F) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t val = 0;
            x86_mem_read32(ctx, ctx->regs.esp, &val);
            ctx->regs.esp += 4;
            fs_write32(ctx, addr, val);
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* PUSH FS:[m] (0x64 0xFF /6) */
        if (op2 == 0xFF) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            if (m.reg == 6) {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                uint32_t val = fs_read32(ctx, addr);
                ctx->regs.esp -= 4;
                x86_mem_write32(ctx, ctx->regs.esp, val);
                ctx->regs.eip += 2 + m.len;
                return true;
            }
        }
    }

    /* Segment override prefixes (0x2E CS, 0x36 SS, 0x3E DS, 0x26 ES) - flat memory model in 32-bit Win32 */
    if (op == 0x2E || op == 0x36 || op == 0x3E || op == 0x26) {
        ctx->regs.eip += 1;
        return true;
    }

    /* LOCK prefix (0xF0) - ignore bus lock in single-threaded emulator */
    if (op == 0xF0) {
        ctx->regs.eip += 1;
        return true;
    }

    /* NOP */
    if (op == 0x90) {
        ctx->regs.eip += 1;
        return true;
    }

    /* CLD (0xFC): Clear Direction Flag */
    if (op == 0xFC) {
        ctx->regs.eflags &= ~X86_EFLAGS_DF;
        ctx->regs.eip += 1;
        return true;
    }

    /* STD (0xFD): Set Direction Flag */
    if (op == 0xFD) {
        ctx->regs.eflags |= X86_EFLAGS_DF;
        ctx->regs.eip += 1;
        return true;
    }

    /* ====== String Operations (Standalone) ====== */
    /* MOVSB (0xA4) */
    if (op == 0xA4) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -1 : 1;
        uint8_t b = 0;
        x86_mem_read8(ctx, ctx->regs.esi, &b);
        x86_mem_write8(ctx, ctx->regs.edi, b);
        ctx->regs.esi += dir;
        ctx->regs.edi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* MOVSD (0xA5) */
    if (op == 0xA5) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -4 : 4;
        uint32_t d = 0;
        x86_mem_read32(ctx, ctx->regs.esi, &d);
        x86_mem_write32(ctx, ctx->regs.edi, d);
        ctx->regs.esi += dir;
        ctx->regs.edi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* CMPSB (0xA6) */
    if (op == 0xA6) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -1 : 1;
        uint8_t b1 = 0, b2 = 0;
        x86_mem_read8(ctx, ctx->regs.esi, &b1);
        x86_mem_read8(ctx, ctx->regs.edi, &b2);
        ctx->regs.esi += dir;
        ctx->regs.edi += dir;
        int diff = (int)b1 - (int)b2;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (diff < 0) ctx->regs.eflags |= (X86_EFLAGS_SF | X86_EFLAGS_CF);
        ctx->regs.eip += 1;
        return true;
    }

    /* CMPSD (0xA7) */
    if (op == 0xA7) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -4 : 4;
        uint32_t d1 = 0, d2 = 0;
        x86_mem_read32(ctx, ctx->regs.esi, &d1);
        x86_mem_read32(ctx, ctx->regs.edi, &d2);
        ctx->regs.esi += dir;
        ctx->regs.edi += dir;
        uint32_t diff = d1 - d2;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (diff & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (d1 < d2) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1;
        return true;
    }


    /* STOSB (0xAA) */
    if (op == 0xAA) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -1 : 1;
        x86_mem_write8(ctx, ctx->regs.edi, (uint8_t)(ctx->regs.eax & 0xFF));
        ctx->regs.edi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* STOSD (0xAB) */
    if (op == 0xAB) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -4 : 4;
        x86_mem_write32(ctx, ctx->regs.edi, ctx->regs.eax);
        ctx->regs.edi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* LODSB (0xAC) */
    if (op == 0xAC) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -1 : 1;
        uint8_t b = 0;
        x86_mem_read8(ctx, ctx->regs.esi, &b);
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | b;
        ctx->regs.esi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* LODSD (0xAD) */
    if (op == 0xAD) {
        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -4 : 4;
        uint32_t d = 0;
        x86_mem_read32(ctx, ctx->regs.esi, &d);
        ctx->regs.eax = d;
        ctx->regs.esi += dir;
        ctx->regs.eip += 1;
        return true;
    }

    /* ====== REP Prefix (0xF3) / REPNZ Prefix (0xF2) ====== */
    if (op == 0xF3 || op == 0xF2) {
        bool is_repnz = (op == 0xF2);
        uint8_t op2 = code[1];
        int prefix_len = 1;
        bool is_16bit = false;
        if (op2 == 0x66) {
            is_16bit = true;
            op2 = code[2];
            prefix_len = 2;
        }

        int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -1 : 1;

        /* REP MOVSB (0xA4) */
        if (op2 == 0xA4) {
            while (ctx->regs.ecx > 0) {
                uint8_t b = 0;
                x86_mem_read8(ctx, ctx->regs.esi, &b);
                x86_mem_write8(ctx, ctx->regs.edi, b);
                ctx->regs.esi += dir;
                ctx->regs.edi += dir;
                ctx->regs.ecx--;
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP MOVSD / MOVSW (0xA5) */
        if (op2 == 0xA5) {
            if (is_16bit) {
                while (ctx->regs.ecx > 0) {
                    uint16_t w = 0;
                    x86_mem_read16(ctx, ctx->regs.esi, &w);
                    x86_mem_write16(ctx, ctx->regs.edi, w);
                    ctx->regs.esi += dir * 2;
                    ctx->regs.edi += dir * 2;
                    ctx->regs.ecx--;
                }
            } else {
                while (ctx->regs.ecx > 0) {
                    uint32_t d = 0;
                    x86_mem_read32(ctx, ctx->regs.esi, &d);
                    x86_mem_write32(ctx, ctx->regs.edi, d);
                    ctx->regs.esi += dir * 4;
                    ctx->regs.edi += dir * 4;
                    ctx->regs.ecx--;
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP STOSB (0xAA) */
        if (op2 == 0xAA) {
            uint8_t val = (uint8_t)(ctx->regs.eax & 0xFF);
            while (ctx->regs.ecx > 0) {
                x86_mem_write8(ctx, ctx->regs.edi, val);
                ctx->regs.edi += dir;
                ctx->regs.ecx--;
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP STOSD / STOSW (0xAB) */
        if (op2 == 0xAB) {
            if (is_16bit) {
                uint16_t val = (uint16_t)(ctx->regs.eax & 0xFFFF);
                while (ctx->regs.ecx > 0) {
                    x86_mem_write16(ctx, ctx->regs.edi, val);
                    ctx->regs.edi += dir * 2;
                    ctx->regs.ecx--;
                }
            } else {
                uint32_t val = ctx->regs.eax;
                while (ctx->regs.ecx > 0) {
                    x86_mem_write32(ctx, ctx->regs.edi, val);
                    ctx->regs.edi += dir * 4;
                    ctx->regs.ecx--;
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP SCASB (0xAE) */
        if (op2 == 0xAE) {
            uint8_t al = (uint8_t)(ctx->regs.eax & 0xFF);
            while (ctx->regs.ecx > 0) {
                uint8_t val = 0;
                x86_mem_read8(ctx, ctx->regs.edi, &val);
                ctx->regs.edi += dir;
                ctx->regs.ecx--;
                int diff = (int)al - (int)val;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (diff < 0) ctx->regs.eflags |= (X86_EFLAGS_SF | X86_EFLAGS_CF);
                if (is_repnz) {
                    if (diff == 0) break;
                } else {
                    if (diff != 0) break;
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP SCASD / SCASW (0xAF) */
        if (op2 == 0xAF) {
            if (is_16bit) {
                uint16_t ax = (uint16_t)(ctx->regs.eax & 0xFFFF);
                while (ctx->regs.ecx > 0) {
                    uint16_t val = 0;
                    x86_mem_read16(ctx, ctx->regs.edi, &val);
                    ctx->regs.edi += dir * 2;
                    ctx->regs.ecx--;
                    int32_t diff = (int32_t)ax - (int32_t)val;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (diff < 0) ctx->regs.eflags |= (X86_EFLAGS_SF | X86_EFLAGS_CF);
                    if (is_repnz) {
                        if (diff == 0) break;
                    } else {
                        if (diff != 0) break;
                    }
                }
            } else {
                uint32_t eax = ctx->regs.eax;
                while (ctx->regs.ecx > 0) {
                    uint32_t val = 0;
                    x86_mem_read32(ctx, ctx->regs.edi, &val);
                    ctx->regs.edi += dir * 4;
                    ctx->regs.ecx--;
                    uint32_t diff = eax - val;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (diff & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    if (eax < val) ctx->regs.eflags |= X86_EFLAGS_CF;
                    if (is_repnz) {
                        if (diff == 0) break;
                    } else {
                        if (diff != 0) break;
                    }
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP CMPSB (0xA6) */
        if (op2 == 0xA6) {
            while (ctx->regs.ecx > 0) {
                uint8_t b1 = 0, b2 = 0;
                x86_mem_read8(ctx, ctx->regs.esi, &b1);
                x86_mem_read8(ctx, ctx->regs.edi, &b2);
                ctx->regs.esi += dir;
                ctx->regs.edi += dir;
                ctx->regs.ecx--;
                int diff = (int)b1 - (int)b2;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (diff < 0) ctx->regs.eflags |= (X86_EFLAGS_SF | X86_EFLAGS_CF);
                if (is_repnz) {
                    if (diff == 0) break;
                } else {
                    if (diff != 0) break;
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        /* REP CMPSD / CMPSW (0xA7) */
        if (op2 == 0xA7) {
            if (is_16bit) {
                while (ctx->regs.ecx > 0) {
                    uint16_t w1 = 0, w2 = 0;
                    x86_mem_read16(ctx, ctx->regs.esi, &w1);
                    x86_mem_read16(ctx, ctx->regs.edi, &w2);
                    ctx->regs.esi += dir * 2;
                    ctx->regs.edi += dir * 2;
                    ctx->regs.ecx--;
                    int32_t diff = (int32_t)w1 - (int32_t)w2;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (diff < 0) ctx->regs.eflags |= (X86_EFLAGS_SF | X86_EFLAGS_CF);
                    if (is_repnz) {
                        if (diff == 0) break;
                    } else {
                        if (diff != 0) break;
                    }
                }
            } else {
                while (ctx->regs.ecx > 0) {
                    uint32_t d1 = 0, d2 = 0;
                    x86_mem_read32(ctx, ctx->regs.esi, &d1);
                    x86_mem_read32(ctx, ctx->regs.edi, &d2);
                    ctx->regs.esi += dir * 4;
                    ctx->regs.edi += dir * 4;
                    ctx->regs.ecx--;
                    uint32_t diff = d1 - d2;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (diff & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    if (d1 < d2) ctx->regs.eflags |= X86_EFLAGS_CF;
                    if (is_repnz) {
                        if (diff == 0) break;
                    } else {
                        if (diff != 0) break;
                    }
                }
            }
            ctx->regs.eip += prefix_len + 1;
            return true;
        }

        if (op2 == 0x90) { /* PAUSE (0xF3 0x90) */
            ctx->regs.eip += prefix_len + 1;
            return true;
        }
        if (op2 == 0xC3) { /* REP RET (0xF3 0xC3) */
            uint32_t ret_addr = 0;
            x86_mem_read32(ctx, ctx->regs.esp, &ret_addr);
            ctx->regs.esp += 4;
            ctx->regs.eip = ret_addr;
            return true;
        }
        if (op2 == 0x0F) { /* 0xF3 0x0F / 0xF2 0x0F SSE2 ops */
            uint8_t op3 = code[prefix_len + 1];
            modrm_t m;
            decode_modrm(code + prefix_len + 2, &m);
            if (op3 == 0x6F || op3 == 0x7E || op3 == 0x10) { /* MOVDQU / MOVQ / MOVSS/SD load */
                int nw = (op3 == 0x6F) ? 4 : (op3 == 0x7E || !is_repnz) ? 2 : 1;
                if (m.mod == 3) {
                    for (int k = 0; k < nw; k++) ctx->xmm[m.reg][k] = ctx->xmm[m.rm][k];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    for (int k = 0; k < nw; k++) x86_mem_read32(ctx, addr + k * 4, &ctx->xmm[m.reg][k]);
                }
                ctx->regs.eip += prefix_len + 2 + m.len;
                return true;
            }
            if (op3 == 0x7F || op3 == 0xD6 || op3 == 0x11) { /* MOVDQU / MOVQ / MOVSS/SD store */
                int nw = (op3 == 0x7F) ? 4 : 2;
                if (m.mod == 3) {
                    for (int k = 0; k < nw; k++) ctx->xmm[m.rm][k] = ctx->xmm[m.reg][k];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    for (int k = 0; k < nw; k++) x86_mem_write32(ctx, addr + k * 4, ctx->xmm[m.reg][k]);
                }
                ctx->regs.eip += prefix_len + 2 + m.len;
                return true;
            }
            ctx->regs.eip += prefix_len + 2 + m.len;
            return true;
        }

        fprintf(stderr, "[REP Prefix] Unhandled sub-opcode 0x%02X at EIP=0x%08X\n", op2, (unsigned int)ctx->regs.eip);
    }

    /* CDQ (0x99): Sign-extend EAX into EDX */
    if (op == 0x99) {
        if (ctx->regs.eax & 0x80000000) {
            ctx->regs.edx = 0xFFFFFFFF;
        } else {
            ctx->regs.edx = 0;
        }
        ctx->regs.eip += 1;
        return true;
    }

    /* ADD EAX, imm32 (0x05) */
    if (op == 0x05) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        uint32_t a = ctx->regs.eax;
        uint32_t res = a + imm;
        ctx->regs.eax = res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (res < a) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (((a ^ ~imm) & (a ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 5;
        return true;
    }

    /* SUB EAX, imm32 (0x2D) */
    if (op == 0x2D) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        uint32_t a = ctx->regs.eax;
        uint32_t res = a - imm;
        ctx->regs.eax = res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (((a ^ imm) & (a ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 5;
        return true;
    }

    /* AND EAX, imm32 (0x25) */
    if (op == 0x25) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        ctx->regs.eax &= imm;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
        if (ctx->regs.eax == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (ctx->regs.eax & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 5;
        return true;
    }

    /* OR EAX, imm32 (0x0D) */
    if (op == 0x0D) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        ctx->regs.eax |= imm;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
        if (ctx->regs.eax == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (ctx->regs.eax & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 5;
        return true;
    }

    /* XOR EAX, imm32 (0x35) */
    if (op == 0x35) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        ctx->regs.eax ^= imm;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
        if (ctx->regs.eax == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (ctx->regs.eax & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 5;
        return true;
    }

    /* CMP EAX, imm32 (0x3D) */
    if (op == 0x3D) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        uint32_t a = ctx->regs.eax, res = a - imm;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 5;
        return true;
    }

    /* TEST EAX, imm32 (0xA9) */
    if (op == 0xA9) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        uint32_t res = ctx->regs.eax & imm;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 5;
        return true;
    }

    /* MOV r32, imm32 (0xB8+r) */
    if (op >= 0xB8 && op <= 0xBF) {
        uint8_t r = op - 0xB8;
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        gprs[r] = imm;
        ctx->regs.eip += 5;
        return true;
    }

    /* INC r32 (0x40+r) */
    if (op >= 0x40 && op <= 0x47) {
        uint8_t r = op - 0x40;
        uint32_t val = ++gprs[r];
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (val == 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 1;
        return true;
    }

    /* DEC r32 (0x48+r) */
    if (op >= 0x48 && op <= 0x4F) {
        uint8_t r = op - 0x48;
        uint32_t val = --gprs[r];
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (val == 0x7FFFFFFF) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 1;
        return true;
    }

    /* PUSH r32 (0x50+r) */
    if (op >= 0x50 && op <= 0x57) {
        uint8_t r = op - 0x50;
        uint32_t val = gprs[r];
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, val);
        ctx->regs.eip += 1;
        return true;
    }

    /* POP r32 (0x58+r) */
    if (op >= 0x58 && op <= 0x5F) {
        uint8_t r = op - 0x58;
        uint32_t val = 0;
        x86_mem_read32(ctx, ctx->regs.esp, &val);
        ctx->regs.esp += 4;
        gprs[r] = val;
        ctx->regs.eip += 1;
        return true;
    }

    /* PUSH imm32 (0x68) */
    if (op == 0x68) {
        uint32_t imm = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                       ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, imm);
        ctx->regs.eip += 5;
        return true;
    }

    /* PUSH imm8 sign-extended (0x6A) */
    if (op == 0x6A) {
        int8_t imm8 = (int8_t)code[1];
        uint32_t imm = (uint32_t)(int32_t)imm8;
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, imm);
        ctx->regs.eip += 2;
        return true;
    }

    /* IMUL r32, r/m32, imm32 (0x69) */
    if (op == 0x69) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                       ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
        int64_t prod = (int64_t)(int32_t)val * (int64_t)(int32_t)imm;
        gprs[m.reg] = (uint32_t)prod;
        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (prod < -2147483648LL || prod > 2147483647LL) {
            ctx->regs.eflags |= (X86_EFLAGS_CF | X86_EFLAGS_OF);
        }
        ctx->regs.eip += 1 + m.len + 4;
        return true;
    }

    /* IMUL r32, r/m32, imm8 (0x6B) */
    if (op == 0x6B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        int32_t imm = (int32_t)(int8_t)code[1 + m.len];
        int64_t prod = (int64_t)(int32_t)val * (int64_t)imm;
        gprs[m.reg] = (uint32_t)prod;
        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (prod < -2147483648LL || prod > 2147483647LL) {
            ctx->regs.eflags |= (X86_EFLAGS_CF | X86_EFLAGS_OF);
        }
        ctx->regs.eip += 1 + m.len + 1;
        return true;
    }

    /* MOV r/m32, r32 (0x89) */
    if (op == 0x89) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.mod == 3) {
            gprs[m.rm] = gprs[m.reg];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_write32(ctx, addr, gprs[m.reg]);
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* MOV r32, r/m32 (0x8B) */
    if (op == 0x8B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.mod == 3) {
            gprs[m.reg] = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t val = 0;
            x86_mem_read32(ctx, addr, &val);
            gprs[m.reg] = val;
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* MOV r/m8, r8 (0x88) */
    if (op == 0x88) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t val = get_reg8(gprs, m.reg);
        if (m.mod == 3) {
            set_reg8(gprs, m.rm, val);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_write8(ctx, addr, val);
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XCHG r/m8, r8 (0x86) */
    if (op == 0x86) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
            set_reg8(gprs, m.rm, a);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
            x86_mem_write8(ctx, addr, a);
        }
        set_reg8(gprs, m.reg, b);
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XCHG r/m32, r32 (0x87) */
    if (op == 0x87) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t a = gprs[m.reg];
        uint32_t b = 0;
        if (m.mod == 3) {
            b = gprs[m.rm];
            gprs[m.rm] = a;
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &b);
            x86_mem_write32(ctx, addr, a);
        }
        gprs[m.reg] = b;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* MOV r8, r/m8 (0x8A) */
    if (op == 0x8A) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t val = 0;
        if (m.mod == 3) {
            val = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &val);
        }
        set_reg8(gprs, m.reg, val);
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* MOV r/m8, imm8 (0xC6 /0) */
    if (op == 0xC6) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) {
            uint8_t imm8 = code[1 + m.len];
            if (m.mod == 3) {
                set_reg8(gprs, m.rm, imm8);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write8(ctx, addr, imm8);
            }
            ctx->regs.eip += 1 + m.len + 1;
            return true;
        }
    }

    /* MOV r8, imm8 (0xB0..0xB7) */
    if (op >= 0xB0 && op <= 0xB7) {
        uint8_t r = op - 0xB0;
        uint8_t imm8 = code[1];
        set_reg8(gprs, r, imm8);
        ctx->regs.eip += 2;
        return true;
    }

    /* ADD r/m8, r8 (0x00) */
    if (op == 0x00) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        uint32_t addr = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a + b;
        if (m.mod == 3) set_reg8(gprs, m.rm, res);
        else x86_mem_write8(ctx, addr, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (res < a) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* ADD r8, r/m8 (0x02) */
    if (op == 0x02) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a + b;
        set_reg8(gprs, m.reg, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (res < a) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* OR r/m8, r8 (0x08) */
    if (op == 0x08) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        uint32_t addr = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a | b;
        if (m.mod == 3) set_reg8(gprs, m.rm, res);
        else x86_mem_write8(ctx, addr, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* OR r8, r/m8 (0x0A) */
    if (op == 0x0A) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a | b;
        set_reg8(gprs, m.reg, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* AND r/m8, r8 (0x20) */
    if (op == 0x20) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        uint32_t addr = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a & b;
        if (m.mod == 3) set_reg8(gprs, m.rm, res);
        else x86_mem_write8(ctx, addr, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* AND r8, r/m8 (0x22) */
    if (op == 0x22) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a & b;
        set_reg8(gprs, m.reg, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SUB r/m8, r8 (0x28) */
    if (op == 0x28) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        uint32_t addr = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a - b;
        if (m.mod == 3) set_reg8(gprs, m.rm, res);
        else x86_mem_write8(ctx, addr, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SUB r8, r/m8 (0x2A) */
    if (op == 0x2A) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a - b;
        set_reg8(gprs, m.reg, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XOR r/m8, r8 (0x30) */
    if (op == 0x30) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        uint32_t addr = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a ^ b;
        if (m.mod == 3) set_reg8(gprs, m.rm, res);
        else x86_mem_write8(ctx, addr, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XOR r8, r/m8 (0x32) */
    if (op == 0x32) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a ^ b;
        set_reg8(gprs, m.reg, res);
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* TEST r/m8, r8 (0x84) */
    if (op == 0x84) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a & b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* Group 1 r/m8, imm8 (0x80) */
    if (op == 0x80) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t imm = code[1 + m.len];
        uint8_t val = 0;
        if (m.mod == 3) {
            val = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &val);
        }
        switch (m.reg) {
            case 0: val += imm; break; /* ADD */
            case 1: val |= imm; break; /* OR */
            case 4: val &= imm; break; /* AND */
            case 5: val -= imm; break; /* SUB */
            case 6: val ^= imm; break; /* XOR */
            case 7: {                  /* CMP */
                uint8_t res = val - imm;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (val < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
                ctx->regs.eip += 1 + m.len + 1;
                return true;
            }
            default: goto unhandled;
        }
        if (m.mod == 3) {
            set_reg8(gprs, m.rm, val);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_write8(ctx, addr, val);
        }
        ctx->regs.eip += 1 + m.len + 1;
        return true;
    }


    /* 0x66 Prefix: 16-bit operand size override */
    if (op == 0x66) {
        uint8_t op2 = code[1];
        /* MOV r/m16, imm16 (0x66 0xC7 /0) */
        if (op2 == 0xC7) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            if (m.reg == 0) {
                uint16_t imm16 = (uint16_t)code[2 + m.len] | ((uint16_t)code[3 + m.len] << 8);
                if (m.mod == 3) {
                    ((uint16_t *)&gprs[m.rm])[0] = imm16;
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    x86_mem_write16(ctx, addr, imm16);
                }
                ctx->regs.eip += 2 + m.len + 2;
                return true;
            }
        }

        /* Group 1 r/m16, imm16 (0x66 0x81) */
        if (op2 == 0x81) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t imm = (uint16_t)code[2 + m.len] | ((uint16_t)code[3 + m.len] << 8);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint32_t res = exec_group1(ctx, m.reg, val, imm, true);
            if (m.reg != 7) {
                if (m.mod == 3) {
                    gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | (res & 0xFFFFU);
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    x86_mem_write16(ctx, addr, (uint16_t)res);
                }
            }
            ctx->regs.eip += 2 + m.len + 2;
            return true;
        }

        /* Group 1 r/m16, imm8 sign-extended (0x66 0x83) */
        if (op2 == 0x83) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t imm = (uint16_t)(int16_t)(int8_t)code[2 + m.len];
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint32_t res = exec_group1(ctx, m.reg, val, imm, true);
            if (m.reg != 7) {
                if (m.mod == 3) {
                    gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | (res & 0xFFFFU);
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    x86_mem_write16(ctx, addr, (uint16_t)res);
                }
            }
            ctx->regs.eip += 2 + m.len + 1;
            return true;
        }

        /* MOV r/m16, r16 (0x66 0x89) */
        if (op2 == 0x89) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = (uint16_t)(gprs[m.reg] & 0xFFFF);
            if (m.mod == 3) {
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | val;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write16(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOV r16, r/m16 (0x66 0x8B) */
        if (op2 == 0x8B) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            gprs[m.reg] = (gprs[m.reg] & ~0xFFFFU) | val;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CMP r/m16, r16 (0x66 0x39) */
        if (op2 == 0x39) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t a = 0;
            if (m.mod == 3) {
                a = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &a);
            }
            uint16_t b = (uint16_t)(gprs[m.reg] & 0xFFFF);
            uint32_t result = (uint32_t)a - (uint32_t)b;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if ((uint16_t)result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (result & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((a ^ b) & (a ^ (uint16_t)result)) & 0x8000) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CMP r16, r/m16 (0x66 0x3B) */
        if (op2 == 0x3B) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t a = (uint16_t)(gprs[m.reg] & 0xFFFF);
            uint16_t b = 0;
            if (m.mod == 3) {
                b = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &b);
            }
            uint32_t result = (uint32_t)a - (uint32_t)b;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if ((uint16_t)result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (result & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((a ^ b) & (a ^ (uint16_t)result)) & 0x8000) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CMP AX, imm16 (0x66 0x3D) */
        if (op2 == 0x3D) {
            uint16_t a = (uint16_t)(ctx->regs.eax & 0xFFFF);
            uint16_t b = (uint16_t)code[2] | ((uint16_t)code[3] << 8);
            uint32_t result = (uint32_t)a - (uint32_t)b;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if ((uint16_t)result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (result & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((a ^ b) & (a ^ (uint16_t)result)) & 0x8000) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 4;
            return true;
        }

        /* TEST r/m16, r16 (0x66 0x85) */
        if (op2 == 0x85) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint16_t result = val & (uint16_t)(gprs[m.reg] & 0xFFFF);
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (result & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* XOR r/m16, r16 (0x66 0x31) */
        if (op2 == 0x31) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = (uint16_t)(gprs[m.reg] & 0xFFFF);
            if (m.mod == 3) {
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | ((gprs[m.rm] ^ val) & 0xFFFFU);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                uint16_t dst = 0;
                x86_mem_read16(ctx, addr, &dst);
                x86_mem_write16(ctx, addr, dst ^ val);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* XOR r16, r/m16 (0x66 0x33) */
        if (op2 == 0x33) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint16_t res = (uint16_t)(gprs[m.reg] & 0xFFFF) ^ val;
            gprs[m.reg] = (gprs[m.reg] & ~0xFFFFU) | res;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* AND r/m16, r16 (0x66 0x21) */
        if (op2 == 0x21) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = (uint16_t)(gprs[m.reg] & 0xFFFF);
            uint16_t dst = 0;
            if (m.mod == 3) {
                dst = (uint16_t)(gprs[m.rm] & 0xFFFF) & val;
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | dst;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &dst);
                dst &= val;
                x86_mem_write16(ctx, addr, dst);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (dst == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (dst & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* AND r16, r/m16 (0x66 0x23) */
        if (op2 == 0x23) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint16_t res = (uint16_t)(gprs[m.reg] & 0xFFFF) & val;
            gprs[m.reg] = (gprs[m.reg] & ~0xFFFFU) | res;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* OR r/m16, r16 (0x66 0x09) */
        if (op2 == 0x09) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = (uint16_t)(gprs[m.reg] & 0xFFFF);
            uint16_t dst = 0;
            if (m.mod == 3) {
                dst = (uint16_t)(gprs[m.rm] & 0xFFFF) | val;
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | dst;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &dst);
                dst |= val;
                x86_mem_write16(ctx, addr, dst);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (dst == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (dst & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* OR r16, r/m16 (0x66 0x0B) */
        if (op2 == 0x0B) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            uint16_t res = (uint16_t)(gprs[m.reg] & 0xFFFF) | val;
            gprs[m.reg] = (gprs[m.reg] & ~0xFFFFU) | res;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* Group 2 r/m16, imm8 (0x66 0xC1) */
        if (op2 == 0xC1) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint8_t count = code[2 + m.len] & 0x1F;
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }

            switch (m.reg) {
                case 0: /* ROL */
                    if (count > 0) {
                        count %= 16;
                        val = (val << count) | (val >> (16 - count));
                    }
                    break;
                case 1: /* ROR */
                    if (count > 0) {
                        count %= 16;
                        val = (val >> count) | (val << (16 - count));
                    }
                    break;
                case 4: /* SHL / SAL */
                    if (count > 0) {
                        val <<= count;
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                case 5: /* SHR (unsigned) */
                    if (count > 0) {
                        val >>= count;
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                case 7: /* SAR (signed) */
                    if (count > 0) {
                        val = (uint16_t)((int16_t)val >> count);
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                default: break;
            }

            if (m.mod == 3) {
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | val;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write16(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len + 1;
            return true;
        }

        /* Group 2 r/m16, 1 (0x66 0xD1) */
        if (op2 == 0xD1) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }

            switch (m.reg) {
                case 0: val = (val << 1) | (val >> 15); break;
                case 1: val = (val >> 1) | (val << 15); break;
                case 4:
                    val <<= 1;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                    if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    break;
                case 5:
                    val >>= 1;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                    if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    break;
                case 7:
                    val = (uint16_t)((int16_t)val >> 1);
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                    if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    break;
                default: break;
            }

            if (m.mod == 3) {
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | val;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write16(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* Group 2 r/m16, CL (0x66 0xD3) */
        if (op2 == 0xD3) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint8_t count = ctx->regs.ecx & 0x1F;
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }

            switch (m.reg) {
                case 4:
                    if (count > 0) {
                        val <<= count;
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                case 5:
                    if (count > 0) {
                        val >>= count;
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                case 7:
                    if (count > 0) {
                        val = (uint16_t)((int16_t)val >> count);
                        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                        if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                        if (val & 0x8000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    }
                    break;
                default: break;
            }

            if (m.mod == 3) {
                gprs[m.rm] = (gprs[m.rm] & ~0xFFFFU) | val;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write16(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOV moffs16, AX (0x66 0xA3) */
        if (op2 == 0xA3) {
            uint32_t addr = (uint32_t)code[2] | ((uint32_t)code[3] << 8) |
                            ((uint32_t)code[4] << 16) | ((uint32_t)code[5] << 24);
            x86_mem_write16(ctx, addr, (uint16_t)ctx->regs.eax);
            ctx->regs.eip += 6;
            return true;
        }

        /* MOV AX, moffs16 (0x66 0xA1) */
        if (op2 == 0xA1) {
            uint32_t addr = (uint32_t)code[2] | ((uint32_t)code[3] << 8) |
                            ((uint32_t)code[4] << 16) | ((uint32_t)code[5] << 24);
            uint16_t val = 0;
            x86_mem_read16(ctx, addr, &val);
            ctx->regs.eax = (ctx->regs.eax & ~0xFFFFU) | val;
            ctx->regs.eip += 6;
            return true;
        }

        /* NOP (0x66 0x90 / XCHG AX, AX) */
        if (op2 == 0x90) {
            ctx->regs.eip += 2;
            return true;
        }

        /* MOVSW (0x66 0xA5) */
        if (op2 == 0xA5) {
            int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -2 : 2;
            uint16_t w = 0;
            x86_mem_read16(ctx, ctx->regs.esi, &w);
            x86_mem_write16(ctx, ctx->regs.edi, w);
            ctx->regs.esi += dir;
            ctx->regs.edi += dir;
            ctx->regs.eip += 2;
            return true;
        }

        /* STOSW (0x66 0xAB) */
        if (op2 == 0xAB) {
            int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -2 : 2;
            x86_mem_write16(ctx, ctx->regs.edi, (uint16_t)(ctx->regs.eax & 0xFFFF));
            ctx->regs.edi += dir;
            ctx->regs.eip += 2;
            return true;
        }

        /* LODSW (0x66 0xAD) */
        if (op2 == 0xAD) {
            int dir = (ctx->regs.eflags & X86_EFLAGS_DF) ? -2 : 2;
            uint16_t w = 0;
            x86_mem_read16(ctx, ctx->regs.esi, &w);
            ctx->regs.eax = (ctx->regs.eax & ~0xFFFFU) | w;
            ctx->regs.esi += dir;
            ctx->regs.eip += 2;
            return true;
        }

        /* SSE2 0x66 0x0F prefix forwarding */
        if (op2 == 0x0F) {
            uint8_t op3 = code[2];
            modrm_t m;
            decode_modrm(code + 3, &m);
            if (op3 == 0x1F) { /* 0x66 0x0F 0x1F multi-byte NOP */
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            if (op3 == 0xEF || op3 == 0x57) { /* PXOR / XORPD xmm1, xmm2/m128 */
                if (m.mod == 3) {
                    for (int k = 0; k < 4; k++) ctx->xmm[m.reg][k] ^= ctx->xmm[m.rm][k];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    for (int k = 0; k < 4; k++) {
                        uint32_t v = 0; x86_mem_read32(ctx, addr + k * 4, &v);
                        ctx->xmm[m.reg][k] ^= v;
                    }
                }
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            if (op3 == 0x6F || op3 == 0x10 || op3 == 0x28 || op3 == 0x12) { /* MOVDQA / MOVUPD / MOVAPD / MOVLPD load */
                int nwords = (op3 == 0x12) ? 2 : 4;
                if (m.mod == 3) {
                    for (int k = 0; k < nwords; k++) ctx->xmm[m.reg][k] = ctx->xmm[m.rm][k];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    for (int k = 0; k < nwords; k++) x86_mem_read32(ctx, addr + k * 4, &ctx->xmm[m.reg][k]);
                }
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            if (op3 == 0x7F || op3 == 0x11 || op3 == 0x29 || op3 == 0xD6 || op3 == 0x13) { /* MOVDQA / MOVUPD / MOVAPD / MOVQ store */
                int nwords = (op3 == 0xD6 || op3 == 0x13) ? 2 : 4;
                if (m.mod == 3) {
                    for (int k = 0; k < nwords; k++) ctx->xmm[m.rm][k] = ctx->xmm[m.reg][k];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    for (int k = 0; k < nwords; k++) x86_mem_write32(ctx, addr + k * 4, ctx->xmm[m.reg][k]);
                }
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            if (op3 == 0x6E) { /* MOVD xmm, r/m32 */
                uint32_t v = (m.mod == 3) ? gprs[m.rm] : 0;
                if (m.mod != 3) x86_mem_read32(ctx, modrm_effective_addr(&m, gprs), &v);
                ctx->xmm[m.reg][0] = v; ctx->xmm[m.reg][1] = 0; ctx->xmm[m.reg][2] = 0; ctx->xmm[m.reg][3] = 0;
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            if (op3 == 0x7E) { /* MOVD r/m32, xmm */
                if (m.mod == 3) gprs[m.rm] = ctx->xmm[m.reg][0];
                else x86_mem_write32(ctx, modrm_effective_addr(&m, gprs), ctx->xmm[m.reg][0]);
                ctx->regs.eip += 3 + m.len;
                return true;
            }
            /* Other 0x66 0x0F SSE2 ops with imm8 (0x70..0x73, 0xC4..0xC6, 0x3A) vs no-imm */
            bool has_imm = (op3 >= 0x70 && op3 <= 0x73) || (op3 >= 0xC4 && op3 <= 0xC6);
            ctx->regs.eip += 3 + m.len + (has_imm ? 1 : 0);
            return true;
        }

        fprintf(stderr, "[0x66 Prefix] Unhandled sub-opcode 0x%02X at EIP=0x%08X (bytes: %02X %02X %02X %02X %02X)\n",
                op2, (unsigned int)ctx->regs.eip, code[0], code[1], code[2], code[3], code[4]);
    }

    /* Group 1 r/m32, imm32 (0x81) */
    if (op == 0x81) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                       ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        uint32_t res = exec_group1(ctx, m.reg, val, imm, false);
        if (m.reg != 7) {
            if (m.mod == 3) {
                gprs[m.rm] = res;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write32(ctx, addr, res);
            }
        }
        ctx->regs.eip += 1 + m.len + 4;
        return true;
    }

    /* Group 1 r/m32, imm8 sign-extended (0x83) */
    if (op == 0x83) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        int8_t imm8 = (int8_t)code[1 + m.len];
        uint32_t imm = (uint32_t)(int32_t)imm8;
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        uint32_t res = exec_group1(ctx, m.reg, val, imm, false);
        if (m.reg != 7) {
            if (m.mod == 3) {
                gprs[m.rm] = res;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write32(ctx, addr, res);
            }
        }
        ctx->regs.eip += 1 + m.len + 1;
        return true;
    }

    /* CMP r/m32, r32 (0x39) */
    if (op == 0x39) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        exec_group1(ctx, 7, val, gprs[m.reg], false);
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* CMP r32, r/m32 (0x3B) */
    if (op == 0x3B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        exec_group1(ctx, 7, gprs[m.reg], val, false);
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* CMP r/m8, r8 (0x38) */
    if (op == 0x38) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = 0;
        if (m.mod == 3) {
            a = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &a);
        }
        uint8_t b = get_reg8(gprs, m.reg);
        uint8_t res = a - b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* CMP r8, r/m8 (0x3A) */
    if (op == 0x3A) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t a = get_reg8(gprs, m.reg);
        uint8_t b = 0;
        if (m.mod == 3) {
            b = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &b);
        }
        uint8_t res = a - b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* ADD AL, imm8 (0x04) */
    if (op == 0x04) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a + b;
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (res < a) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 2;
        return true;
    }

    /* OR AL, imm8 (0x0C) */
    if (op == 0x0C) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a | b;
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 2;
        return true;
    }

    /* AND AL, imm8 (0x24) */
    if (op == 0x24) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a & b;
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 2;
        return true;
    }

    /* SUB AL, imm8 (0x2C) */
    if (op == 0x2C) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a - b;
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 2;
        return true;
    }

    /* XOR AL, imm8 (0x34) */
    if (op == 0x34) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a ^ b;
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | res;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 2;
        return true;
    }

    /* CMP AL, imm8 (0x3C) */
    if (op == 0x3C) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a - b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        ctx->regs.eip += 2;
        return true;
    }

    /* TEST AL, imm8 (0xA8) */
    if (op == 0xA8) {
        uint8_t a = (uint8_t)(ctx->regs.eax & 0xFF);
        uint8_t b = code[1];
        uint8_t res = a & b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 2;
        return true;
    }

    /* MOV r/m32, imm32 (0xC7 /0) */
    if (op == 0xC7) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) {
            uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                           ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
            if (m.mod == 3) {
                gprs[m.rm] = imm;
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write32(ctx, addr, imm);
            }
            ctx->regs.eip += 1 + m.len + 4;
            return true;
        }
    }

    /* Group 3 r/m8 (0xF6) */
    if (op == 0xF6) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t val = 0;
        if (m.mod == 3) {
            val = get_reg8(gprs, m.rm);
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &val);
        }

        if (m.reg == 0 || m.reg == 1) { /* TEST r/m8, imm8 */
            uint8_t imm8 = code[1 + m.len];
            uint8_t res = val & imm8;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 1 + m.len + 1;
            return true;
        } else if (m.reg == 2) { /* NOT r/m8 */
            val = ~val;
            if (m.mod == 3) set_reg8(gprs, m.rm, val);
            else x86_mem_write8(ctx, modrm_effective_addr(&m, gprs), val);
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 3) { /* NEG r/m8 */
            uint8_t res = (uint8_t)(-(int8_t)val);
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val != 0) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (m.mod == 3) set_reg8(gprs, m.rm, res);
            else x86_mem_write8(ctx, modrm_effective_addr(&m, gprs), res);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    /* Group 3 r/m32 (0xF7) */
    if (op == 0xF7) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }

        if (m.reg == 0 || m.reg == 1) { /* TEST r/m32, imm32 */
            uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                           ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
            uint32_t res = val & imm;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            ctx->regs.eip += 1 + m.len + 4;
            return true;
        } else if (m.reg == 2) { /* NOT r/m32 */
            val = ~val;
            if (m.mod == 3) gprs[m.rm] = val;
            else x86_mem_write32(ctx, modrm_effective_addr(&m, gprs), val);
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 3) { /* NEG r/m32 */
            uint32_t res = 0 - val;
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val != 0) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (m.mod == 3) gprs[m.rm] = res;
            else x86_mem_write32(ctx, modrm_effective_addr(&m, gprs), res);
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 4) { /* MUL r/m32 */
            uint64_t prod = (uint64_t)ctx->regs.eax * (uint64_t)val;
            ctx->regs.eax = (uint32_t)prod;
            ctx->regs.edx = (uint32_t)(prod >> 32);
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 5) { /* IMUL r/m32 */
            int64_t prod = (int64_t)(int32_t)ctx->regs.eax * (int64_t)(int32_t)val;
            ctx->regs.eax = (uint32_t)prod;
            ctx->regs.edx = (uint32_t)((uint64_t)prod >> 32);
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 6) { /* DIV r/m32 */
            if (val != 0) {
                uint64_t dividend = ((uint64_t)ctx->regs.edx << 32) | (uint64_t)ctx->regs.eax;
                ctx->regs.eax = (uint32_t)(dividend / val);
                ctx->regs.edx = (uint32_t)(dividend % val);
            }
            ctx->regs.eip += 1 + m.len;
            return true;
        } else if (m.reg == 7) { /* IDIV r/m32 */
            if (val != 0) {
                int64_t dividend = (int64_t)(((uint64_t)ctx->regs.edx << 32) | (uint64_t)ctx->regs.eax);
                int32_t divisor = (int32_t)val;
                ctx->regs.eax = (uint32_t)(dividend / divisor);
                ctx->regs.edx = (uint32_t)(dividend % divisor);
            }
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    /* MOV AL, [moffs8] (0xA0) */
    if (op == 0xA0) {
        uint32_t addr = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                        ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        uint8_t b = 0;
        x86_mem_read8(ctx, addr, &b);
        ctx->regs.eax = (ctx->regs.eax & ~0xFFU) | b;
        ctx->regs.eip += 5;
        return true;
    }

    /* MOV [moffs8], AL (0xA2) */
    if (op == 0xA2) {
        uint32_t addr = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                        ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        x86_mem_write8(ctx, addr, (uint8_t)(ctx->regs.eax & 0xFF));
        ctx->regs.eip += 5;
        return true;
    }

    /* MOV EAX, [moffs32] (0xA1) */
    if (op == 0xA1) {
        uint32_t addr = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                        ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        x86_mem_read32(ctx, addr, &ctx->regs.eax);
        ctx->regs.eip += 5;
        return true;
    }

    /* MOV [moffs32], EAX (0xA3) */
    if (op == 0xA3) {
        uint32_t addr = (uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                        ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24);
        x86_mem_write32(ctx, addr, ctx->regs.eax);
        ctx->regs.eip += 5;
        return true;
    }

    /* LEA r32, [r/m] (0x8D) */
    if (op == 0x8D) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        gprs[m.reg] = modrm_effective_addr(&m, gprs);
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XOR r/m32, r32 (0x31) */
    if (op == 0x31) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src = gprs[m.reg];
        uint32_t dst;
        uint32_t addr = 0;
        if (m.mod == 3) {
            dst = gprs[m.rm];
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &dst);
        }
        uint32_t res = dst ^ src;
        if (m.mod == 3) gprs[m.rm] = res;
        else x86_mem_write32(ctx, addr, res);

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XOR r32, r/m32 (0x33) */
    if (op == 0x33) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src;
        if (m.mod == 3) {
            src = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &src);
        }
        uint32_t res = gprs[m.reg] ^ src;
        gprs[m.reg] = res;

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* ADD r/m32, r32 (0x01) */
    if (op == 0x01) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.mod == 3) {
            gprs[m.rm] += gprs[m.reg];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t val = 0;
            x86_mem_read32(ctx, addr, &val);
            val += gprs[m.reg];
            x86_mem_write32(ctx, addr, val);
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* ADD r32, r/m32 (0x03) */
    if (op == 0x03) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.mod == 3) {
            gprs[m.reg] += gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t val = 0;
            x86_mem_read32(ctx, addr, &val);
            gprs[m.reg] += val;
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SUB r/m32, r32 (0x29) - reg/reg */
    if (op == 0x29) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.mod == 3) {
            gprs[m.rm] -= gprs[m.reg];
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    /* SUB r32, r/m32 (0x2B) - reg/reg */
    /* SBB r32, r/m32 (0x1B) */
    if (op == 0x1B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src;
        if (m.mod == 3) {
            src = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &src);
        }
        uint32_t dst = gprs[m.reg];
        uint32_t borrow = (ctx->regs.eflags & X86_EFLAGS_CF) ? 1 : 0;
        uint64_t diff = (uint64_t)dst - (uint64_t)src - (uint64_t)borrow;
        uint32_t res = (uint32_t)diff;
        gprs[m.reg] = res;

        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (diff > 0xFFFFFFFFULL) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (((dst ^ src) & (dst ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;

        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SBB r/m32, r32 (0x19) */
    if (op == 0x19) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src = gprs[m.reg];
        uint32_t dst;
        uint32_t addr = 0;
        if (m.mod == 3) {
            dst = gprs[m.rm];
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &dst);
        }
        uint32_t borrow = (ctx->regs.eflags & X86_EFLAGS_CF) ? 1 : 0;
        uint64_t diff = (uint64_t)dst - (uint64_t)src - (uint64_t)borrow;
        uint32_t res = (uint32_t)diff;

        if (m.mod == 3) gprs[m.rm] = res;
        else x86_mem_write32(ctx, addr, res);

        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (diff > 0xFFFFFFFFULL) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (((dst ^ src) & (dst ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;

        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SUB r/m32, r32 (0x29) */
    if (op == 0x29) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src = gprs[m.reg];
        uint32_t dst;
        uint32_t addr = 0;
        if (m.mod == 3) {
            dst = gprs[m.rm];
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &dst);
        }
        uint32_t res = dst - src;
        if (m.mod == 3) gprs[m.rm] = res;
        else x86_mem_write32(ctx, addr, res);

        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (dst < src) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (((dst ^ src) & (dst ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;

        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* SUB r32, r/m32 (0x2B) */
    if (op == 0x2B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src;
        if (m.mod == 3) {
            src = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &src);
        }
        uint32_t dst = gprs[m.reg];
        uint32_t res = dst - src;
        gprs[m.reg] = res;

        ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
        if (dst < src) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (((dst ^ src) & (dst ^ res)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;

        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* AND r/m32, r32 (0x21) */
    if (op == 0x21) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src = gprs[m.reg];
        uint32_t dst;
        uint32_t addr = 0;
        if (m.mod == 3) {
            dst = gprs[m.rm];
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &dst);
        }
        uint32_t res = dst & src;
        if (m.mod == 3) gprs[m.rm] = res;
        else x86_mem_write32(ctx, addr, res);

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* AND r32, r/m32 (0x23) */
    if (op == 0x23) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src;
        if (m.mod == 3) {
            src = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &src);
        }
        uint32_t res = gprs[m.reg] & src;
        gprs[m.reg] = res;

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* OR r/m32, r32 (0x09) */
    if (op == 0x09) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src = gprs[m.reg];
        uint32_t dst;
        uint32_t addr = 0;
        if (m.mod == 3) {
            dst = gprs[m.rm];
        } else {
            addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &dst);
        }
        uint32_t res = dst | src;
        if (m.mod == 3) gprs[m.rm] = res;
        else x86_mem_write32(ctx, addr, res);

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* OR r32, r/m32 (0x0B) */
    if (op == 0x0B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t src;
        if (m.mod == 3) {
            src = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &src);
        }
        uint32_t res = gprs[m.reg] | src;
        gprs[m.reg] = res;

        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* TEST r/m32, r32 (0x85) */
    if (op == 0x85) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }
        uint32_t result = val & gprs[m.reg];
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* CMP r/m32, r32 (0x39) */
    if (op == 0x39) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t a = 0;
        if (m.mod == 3) {
            a = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &a);
        }
        uint32_t b = gprs[m.reg];
        uint32_t result = a - b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (((a ^ b) & (a ^ result)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* CMP r32, r/m32 (0x3B) */
    if (op == 0x3B) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t a = gprs[m.reg];
        uint32_t b = 0;
        if (m.mod == 3) {
            b = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &b);
        }
        uint32_t result = a - b;
        ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
        if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
        if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
        if (a < b) ctx->regs.eflags |= X86_EFLAGS_CF;
        if (((a ^ b) & (a ^ result)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* Group 1 r/m32, imm32 (0x81) - ADD/OR/AND/SUB/XOR/CMP */
    if (op == 0x81) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                       ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
        if (m.mod == 3) {
            switch (m.reg) {
                case 0: gprs[m.rm] += imm; break;                      /* ADD */
                case 1: gprs[m.rm] |= imm; break;                      /* OR */
                case 4: gprs[m.rm] &= imm; break;                      /* AND */
                case 5: gprs[m.rm] -= imm; break;                      /* SUB */
                case 6: gprs[m.rm] ^= imm; break;                      /* XOR */
                case 7: {                                               /* CMP */
                    uint32_t a = gprs[m.rm], result = a - imm;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    if (a < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
                    break;
                }
                default: goto unhandled;
            }
            ctx->regs.eip += 1 + m.len + 4;
            return true;
        }
    }

    /* Group 1 r/m32, imm8 (0x83) sign-extended - ADD/OR/AND/SUB/XOR/CMP */
    if (op == 0x83) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t imm = (uint32_t)(int32_t)(int8_t)code[1 + m.len];
        if (m.mod == 3) {
            switch (m.reg) {
                case 0: gprs[m.rm] += imm; break;                      /* ADD */
                case 1: gprs[m.rm] |= imm; break;                      /* OR */
                case 4: gprs[m.rm] &= imm; break;                      /* AND */
                case 5: gprs[m.rm] -= imm; break;                      /* SUB */
                case 6: gprs[m.rm] ^= imm; break;                      /* XOR */
                case 7: {                                               /* CMP */
                    uint32_t a = gprs[m.rm], result = a - imm;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    if (a < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
                    break;
                }
                default: goto unhandled;
            }
            ctx->regs.eip += 1 + m.len + 1;
            return true;
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t val = 0;
            x86_mem_read32(ctx, addr, &val);
            switch (m.reg) {
                case 0: val += imm; x86_mem_write32(ctx, addr, val); break; /* ADD */
                case 1: val |= imm; x86_mem_write32(ctx, addr, val); break; /* OR */
                case 4: val &= imm; x86_mem_write32(ctx, addr, val); break; /* AND */
                case 5: val -= imm; x86_mem_write32(ctx, addr, val); break; /* SUB */
                case 6: val ^= imm; x86_mem_write32(ctx, addr, val); break; /* XOR */
                case 7: {                                               /* CMP */
                    uint32_t a = val, result = a - imm;
                    ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                    if (result == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                    if (result & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                    if (a < imm) ctx->regs.eflags |= X86_EFLAGS_CF;
                    break;
                }
                default: goto unhandled;
            }
            ctx->regs.eip += 1 + m.len + 1;
            return true;
        }
    }

    /* Group 2 r/m32, imm8 (0xC1) - ROL / ROR / RCL / RCR / SHL / SHR / SAR */
    if (op == 0xC1) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t count = code[1 + m.len] & 0x1F;
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }

        switch (m.reg) {
            case 0: val = (val << count) | (val >> (32 - count)); break; /* ROL */
            case 1: val = (val >> count) | (val << (32 - count)); break; /* ROR */
            case 4: /* SHL / SAL */
                val <<= count;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            case 5: /* SHR (unsigned) */
                val >>= count;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            case 7: /* SAR (signed) */
                val = (uint32_t)((int32_t)val >> count);
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            default: goto unhandled;
        }

        if (m.mod == 3) gprs[m.rm] = val;
        else { uint32_t addr = modrm_effective_addr(&m, gprs); x86_mem_write32(ctx, addr, val); }
        ctx->regs.eip += 1 + m.len + 1;
        return true;
    }

    /* Group 2 r/m32, CL (0xD3) */
    if (op == 0xD3) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t count = ctx->regs.ecx & 0x1F;
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }

        switch (m.reg) {
            case 4: val <<= count; break;
            case 5: val >>= count; break;
            case 7: val = (uint32_t)((int32_t)val >> count); break;
            default: goto unhandled;
        }

        if (m.mod == 3) gprs[m.rm] = val;
        else { uint32_t addr = modrm_effective_addr(&m, gprs); x86_mem_write32(ctx, addr, val); }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* Group 2 r/m32, 1 (0xD1) */
    if (op == 0xD1) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }

        switch (m.reg) {
            case 0: { /* ROL */
                uint32_t bit = (val >> 31) & 1;
                val = (val << 1) | bit;
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (((val >> 31) & 1) ^ bit) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 1: { /* ROR */
                uint32_t bit = val & 1;
                val = (val >> 1) | (bit << 31);
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (((val >> 31) ^ (val >> 30)) & 1) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 2: { /* RCL */
                uint32_t cf = (ctx->regs.eflags & X86_EFLAGS_CF) ? 1 : 0;
                uint32_t bit = (val >> 31) & 1;
                val = (val << 1) | cf;
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (((val >> 31) & 1) ^ bit) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 3: { /* RCR */
                uint32_t cf = (ctx->regs.eflags & X86_EFLAGS_CF) ? 1 : 0;
                uint32_t bit = val & 1;
                val = (val >> 1) | (cf << 31);
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (((val >> 31) ^ (val >> 30)) & 1) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 4: { /* SHL / SAL */
                uint32_t bit = (val >> 31) & 1;
                val <<= 1;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (((val >> 31) & 1) ^ bit) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 5: { /* SHR */
                uint32_t bit = val & 1;
                val >>= 1;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
                break;
            }
            case 7: { /* SAR */
                uint32_t bit = val & 1;
                val = (uint32_t)((int32_t)val >> 1);
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (bit) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            }
            default: goto unhandled;
        }

        if (m.mod == 3) gprs[m.rm] = val;
        else { uint32_t addr = modrm_effective_addr(&m, gprs); x86_mem_write32(ctx, addr, val); }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* Group 2 r/m8, 1 (0xD0) */
    if (op == 0xD0) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint8_t val = 0;
        if (m.mod == 3) {
            val = ((uint8_t *)&gprs[m.rm & 3])[m.rm >= 4 ? 1 : 0];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read8(ctx, addr, &val);
        }

        switch (m.reg) {
            case 0: val = (val << 1) | (val >> 7); break; /* ROL */
            case 1: val = (val >> 1) | (val << 7); break; /* ROR */
            case 4: /* SHL / SAL */
                val <<= 1;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            case 5: /* SHR */
                val >>= 1;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            case 7: /* SAR */
                val = (uint8_t)((int8_t)val >> 1);
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (val & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
                break;
            default: goto unhandled;
        }

        if (m.mod == 3) {
            ((uint8_t *)&gprs[m.rm & 3])[m.rm >= 4 ? 1 : 0] = val;
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_write8(ctx, addr, val);
        }
        ctx->regs.eip += 1 + m.len;
        return true;
    }

    /* XCHG EAX, r32 (0x91..0x97) */
    if (op >= 0x91 && op <= 0x97) {
        uint8_t r = op - 0x90;
        uint32_t tmp = gprs[0];
        gprs[0] = gprs[r];
        gprs[r] = tmp;
        ctx->regs.eip += 1;
        return true;
    }

    /* ====== Conditional Jumps (Jcc) - short 8-bit displacement ====== */
    /* JE/JZ (0x74) */
    if (op == 0x74) {
        int8_t disp = (int8_t)code[1];
        if (ctx->regs.eflags & X86_EFLAGS_ZF)
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JNE/JNZ (0x75) */
    if (op == 0x75) {
        int8_t disp = (int8_t)code[1];
        if (!(ctx->regs.eflags & X86_EFLAGS_ZF))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JBE/JNA (0x76) - CF==1 or ZF==1 */
    if (op == 0x76) {
        int8_t disp = (int8_t)code[1];
        if (ctx->regs.eflags & (X86_EFLAGS_CF | X86_EFLAGS_ZF))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JA/JNBE (0x77) - CF==0 and ZF==0 */
    if (op == 0x77) {
        int8_t disp = (int8_t)code[1];
        if (!(ctx->regs.eflags & (X86_EFLAGS_CF | X86_EFLAGS_ZF)))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JL/JNGE (0x7C) - SF != OF */
    if (op == 0x7C) {
        int8_t disp = (int8_t)code[1];
        bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
        bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
        if (sf != of)
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JGE/JNL (0x7D) - SF == OF */
    if (op == 0x7D) {
        int8_t disp = (int8_t)code[1];
        bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
        bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
        if (sf == of)
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JLE/JNG (0x7E) - ZF==1 or SF!=OF */
    if (op == 0x7E) {
        int8_t disp = (int8_t)code[1];
        bool zf = !!(ctx->regs.eflags & X86_EFLAGS_ZF);
        bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
        bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
        if (zf || (sf != of))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JG/JNLE (0x7F) - ZF==0 and SF==OF */
    if (op == 0x7F) {
        int8_t disp = (int8_t)code[1];
        bool zf = !!(ctx->regs.eflags & X86_EFLAGS_ZF);
        bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
        bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
        if (!zf && (sf == of))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JB/JNAE/JC (0x72) - CF==1 */
    if (op == 0x72) {
        int8_t disp = (int8_t)code[1];
        if (ctx->regs.eflags & X86_EFLAGS_CF)
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JAE/JNB/JNC (0x73) - CF==0 */
    if (op == 0x73) {
        int8_t disp = (int8_t)code[1];
        if (!(ctx->regs.eflags & X86_EFLAGS_CF))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JS (0x78) - SF==1 */
    if (op == 0x78) {
        int8_t disp = (int8_t)code[1];
        if (ctx->regs.eflags & X86_EFLAGS_SF)
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }
    /* JNS (0x79) - SF==0 */
    if (op == 0x79) {
        int8_t disp = (int8_t)code[1];
        if (!(ctx->regs.eflags & X86_EFLAGS_SF))
            ctx->regs.eip += 2 + disp;
        else
            ctx->regs.eip += 2;
        return true;
    }

    /* ====== Two-byte Opcode Prefix (0x0F) ====== */
    if (op == 0x0F) {
        uint8_t op2 = code[1];

        /* Jcc rel32 (0x0F 0x80..0x8F) */
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t disp32 = (int32_t)((uint32_t)code[2] | ((uint32_t)code[3] << 8) |
                                       ((uint32_t)code[4] << 16) | ((uint32_t)code[5] << 24));
            bool take = false;
            bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
            bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
            bool zf = !!(ctx->regs.eflags & X86_EFLAGS_ZF);
            bool cf = !!(ctx->regs.eflags & X86_EFLAGS_CF);
            switch (op2) {
                case 0x84: take = zf; break;             /* JE/JZ */
                case 0x85: take = !zf; break;            /* JNE/JNZ */
                case 0x8C: take = (sf != of); break;     /* JL */
                case 0x8D: take = (sf == of); break;     /* JGE */
                case 0x8E: take = (zf || (sf != of)); break; /* JLE */
                case 0x8F: take = (!zf && (sf == of)); break; /* JG */
                case 0x82: take = cf; break;             /* JB */
                case 0x83: take = !cf; break;            /* JAE */
                case 0x86: take = (cf || zf); break;     /* JBE */
                case 0x87: take = (!cf && !zf); break;   /* JA */
                default: break;
            }
            if (take)
                ctx->regs.eip += 6 + disp32;
            else
                ctx->regs.eip += 6;
            return true;
        }

        /* CMOVcc r32, r/m32 (0x0F 0x40..0x4F) */
        if (op2 >= 0x40 && op2 <= 0x4F) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            bool take = false;
            bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
            bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
            bool zf = !!(ctx->regs.eflags & X86_EFLAGS_ZF);
            bool cf = !!(ctx->regs.eflags & X86_EFLAGS_CF);
            switch (op2) {
                case 0x44: take = zf; break;                 /* CMOVE/CMOVZ */
                case 0x45: take = !zf; break;                /* CMOVNE/CMOVNZ */
                case 0x4C: take = (sf != of); break;         /* CMOVL */
                case 0x4D: take = (sf == of); break;         /* CMOVGE */
                case 0x4E: take = (zf || (sf != of)); break; /* CMOVLE */
                case 0x4F: take = (!zf && (sf == of)); break;/* CMOVG */
                case 0x42: take = cf; break;                 /* CMOVB */
                case 0x43: take = !cf; break;                /* CMOVAE */
                case 0x46: take = (cf || zf); break;         /* CMOVBE */
                case 0x47: take = (!cf && !zf); break;       /* CMOVA */
                case 0x48: take = sf; break;                 /* CMOVS */
                case 0x49: take = !sf; break;                /* CMOVNS */
                default: break;
            }
            if (take) {
                uint32_t val = 0;
                if (m.mod == 3) {
                    val = gprs[m.rm];
                } else {
                    uint32_t addr = modrm_effective_addr(&m, gprs);
                    x86_mem_read32(ctx, addr, &val);
                }
                gprs[m.reg] = val;
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* SETcc r/m8 (0x0F 0x90..0x9F) */
        if (op2 >= 0x90 && op2 <= 0x9F) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            bool take = false;
            bool sf = !!(ctx->regs.eflags & X86_EFLAGS_SF);
            bool of = !!(ctx->regs.eflags & X86_EFLAGS_OF);
            bool zf = !!(ctx->regs.eflags & X86_EFLAGS_ZF);
            bool cf = !!(ctx->regs.eflags & X86_EFLAGS_CF);
            switch (op2) {
                case 0x94: take = zf; break;                 /* SETE/SETZ */
                case 0x95: take = !zf; break;                /* SETNE/SETNZ */
                case 0x9C: take = (sf != of); break;         /* SETL */
                case 0x9D: take = (sf == of); break;         /* SETGE */
                case 0x9E: take = (zf || (sf != of)); break; /* SETLE */
                case 0x9F: take = (!zf && (sf == of)); break;/* SETG */
                case 0x92: take = cf; break;                 /* SETB */
                case 0x93: take = !cf; break;                /* SETAE */
                case 0x96: take = (cf || zf); break;         /* SETBE */
                case 0x97: take = (!cf && !zf); break;       /* SETA */
                default: break;
            }
            uint8_t res = take ? 1 : 0;
            if (m.mod == 3) {
                set_reg8(gprs, m.rm, res);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_write8(ctx, addr, res);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVZX r32, r/m8 (0x0F 0xB6) */
        if (op2 == 0xB6) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint8_t val = 0;
            if (m.mod == 3) {
                val = get_reg8(gprs, m.rm);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read8(ctx, addr, &val);
            }
            gprs[m.reg] = (uint32_t)val;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVZX r32, r/m16 (0x0F 0xB7) */
        if (op2 == 0xB7) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint16_t val = 0;
            if (m.mod == 3) {
                val = (uint16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read16(ctx, addr, &val);
            }
            gprs[m.reg] = (uint32_t)val;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVSX r32, r/m8 (0x0F 0xBE) */
        if (op2 == 0xBE) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            int8_t val = 0;
            if (m.mod == 3) {
                val = (int8_t)get_reg8(gprs, m.rm);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                uint8_t u8 = 0;
                x86_mem_read8(ctx, addr, &u8);
                val = (int8_t)u8;
            }
            gprs[m.reg] = (uint32_t)(int32_t)val;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVSX r32, r/m16 (0x0F 0xBF) */
        if (op2 == 0xBF) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            int16_t val = 0;
            if (m.mod == 3) {
                val = (int16_t)(gprs[m.rm] & 0xFFFF);
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                uint16_t u16 = 0;
                x86_mem_read16(ctx, addr, &u16);
                val = (int16_t)u16;
            }
            gprs[m.reg] = (uint32_t)(int32_t)val;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* IMUL r32, r/m32 (0x0F 0xAF) */
        if (op2 == 0xAF) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t val = 0;
            if (m.mod == 3) {
                val = gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &val);
            }
            int64_t res = (int64_t)(int32_t)gprs[m.reg] * (int64_t)(int32_t)val;
            gprs[m.reg] = (uint32_t)res;
            ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res < -2147483648LL || res > 2147483647LL) {
                ctx->regs.eflags |= (X86_EFLAGS_CF | X86_EFLAGS_OF);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CMPXCHG r/m8, r8 (0x0F 0xB0) */
        if (op2 == 0xB0) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint8_t dst = 0;
            uint32_t addr = 0;
            if (m.mod == 3) {
                dst = get_reg8(gprs, m.rm);
            } else {
                addr = modrm_effective_addr(&m, gprs);
                x86_mem_read8(ctx, addr, &dst);
            }
            uint8_t al = (uint8_t)(ctx->regs.eax & 0xFF);
            uint8_t src = get_reg8(gprs, m.reg);
            uint8_t diff = al - dst;

            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (diff & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (al < dst) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((al ^ dst) & (al ^ diff)) & 0x80) ctx->regs.eflags |= X86_EFLAGS_OF;

            if (al == dst) {
                if (m.mod == 3) set_reg8(gprs, m.rm, src);
                else x86_mem_write8(ctx, addr, src);
            } else {
                set_reg8(gprs, 0 /* AL */, dst);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CMPXCHG r/m32, r32 (0x0F 0xB1) */
        if (op2 == 0xB1) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t dst = 0;
            uint32_t addr = 0;
            if (m.mod == 3) {
                dst = gprs[m.rm];
            } else {
                addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &dst);
            }
            uint32_t eax = ctx->regs.eax;
            uint32_t src = gprs[m.reg];
            uint32_t diff = eax - dst;

            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (diff == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (diff & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (eax < dst) ctx->regs.eflags |= X86_EFLAGS_CF;
            if (((eax ^ dst) & (eax ^ diff)) & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;

            if (eax == dst) {
                if (m.mod == 3) gprs[m.rm] = src;
                else x86_mem_write32(ctx, addr, src);
            } else {
                ctx->regs.eax = dst;
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* Multi-byte NOP (0x0F 0x1F /0) */
        if (op2 == 0x1F) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* CPUID (0x0F 0xA2) */
        if (op2 == 0xA2) {
            ctx->regs.eax = 1;
            ctx->regs.ebx = 0;
            ctx->regs.ecx = 0;
            ctx->regs.edx = (1U << 25) | (1U << 26); /* SSE + SSE2 */
            ctx->regs.eip += 2;
            return true;
        }

        /* RDTSC (0x0F 0x31) */
        if (op2 == 0x31) {
            ctx->regs.eax = (uint32_t)(ctx->instructions_run * 16 + 0x12345678U);
            ctx->regs.edx = (uint32_t)((ctx->instructions_run * 16) >> 32);
            ctx->regs.eip += 2;
            return true;
        }

        /* XADD r/m32, r32 (0x0F 0xC1) */
        if (op2 == 0xC1) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t src = gprs[m.reg], dst = 0, addr = 0;
            if (m.mod == 3) dst = gprs[m.rm];
            else { addr = modrm_effective_addr(&m, gprs); x86_mem_read32(ctx, addr, &dst); }
            uint32_t res = dst + src;
            gprs[m.reg] = dst;
            if (m.mod == 3) gprs[m.rm] = res;
            else x86_mem_write32(ctx, addr, res);
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
            if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (res < dst) ctx->regs.eflags |= X86_EFLAGS_CF;
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* BT / BTS / BTR / BTC r/m32, r32 (0x0F 0xA3 / 0xAB / 0xB3 / 0xBB) */
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t bit = gprs[m.reg] & 31, val = 0, addr = 0;
            if (m.mod == 3) val = gprs[m.rm];
            else { addr = modrm_effective_addr(&m, gprs); x86_mem_read32(ctx, addr, &val); }
            if (val & (1U << bit)) ctx->regs.eflags |= X86_EFLAGS_CF;
            else ctx->regs.eflags &= ~X86_EFLAGS_CF;
            if (op2 == 0xAB) val |= (1U << bit);
            else if (op2 == 0xB3) val &= ~(1U << bit);
            else if (op2 == 0xBB) val ^= (1U << bit);
            if (op2 != 0xA3) {
                if (m.mod == 3) gprs[m.rm] = val;
                else x86_mem_write32(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* SHLD r/m32, r32, imm8 / CL (0x0F 0xA4 / 0x0F 0xA5) */
        if (op2 == 0xA4 || op2 == 0xA5) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t src = gprs[m.reg];
            uint32_t dst = 0, addr = 0;
            if (m.mod == 3) dst = gprs[m.rm];
            else { addr = modrm_effective_addr(&m, gprs); x86_mem_read32(ctx, addr, &dst); }
            uint8_t count = (op2 == 0xA4) ? code[2 + m.len] : (uint8_t)(ctx->regs.ecx & 0xFF);
            count &= 31;
            if (count > 0) {
                uint32_t res = (dst << count) | (src >> (32 - count));
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if ((dst >> (32 - count)) & 1) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (m.mod == 3) gprs[m.rm] = res;
                else x86_mem_write32(ctx, addr, res);
            }
            ctx->regs.eip += 2 + m.len + (op2 == 0xA4 ? 1 : 0);
            return true;
        }

        /* SHRD r/m32, r32, imm8 / CL (0x0F 0xAC / 0x0F 0xAD) */
        if (op2 == 0xAC || op2 == 0xAD) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint32_t src = gprs[m.reg];
            uint32_t dst = 0, addr = 0;
            if (m.mod == 3) dst = gprs[m.rm];
            else { addr = modrm_effective_addr(&m, gprs); x86_mem_read32(ctx, addr, &dst); }
            uint8_t count = (op2 == 0xAC) ? code[2 + m.len] : (uint8_t)(ctx->regs.ecx & 0xFF);
            count &= 31;
            if (count > 0) {
                uint32_t res = (dst >> count) | (src << (32 - count));
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_ZF | X86_EFLAGS_SF);
                if ((dst >> (count - 1)) & 1) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (m.mod == 3) gprs[m.rm] = res;
                else x86_mem_write32(ctx, addr, res);
            }
            ctx->regs.eip += 2 + m.len + (op2 == 0xAC ? 1 : 0);
            return true;
        }

        /* BT / BTS / BTR / BTC r/m32, imm8 (0x0F 0xBA) */
        if (op2 == 0xBA) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            uint8_t bit = code[2 + m.len] & 31;
            uint32_t val = 0, addr = 0;
            if (m.mod == 3) val = gprs[m.rm];
            else { addr = modrm_effective_addr(&m, gprs); x86_mem_read32(ctx, addr, &val); }
            if (val & (1U << bit)) ctx->regs.eflags |= X86_EFLAGS_CF;
            else ctx->regs.eflags &= ~X86_EFLAGS_CF;
            if (m.reg == 5) val |= (1U << bit);
            else if (m.reg == 6) val &= ~(1U << bit);
            else if (m.reg == 7) val ^= (1U << bit);
            if (m.reg >= 5) {
                if (m.mod == 3) gprs[m.rm] = val;
                else x86_mem_write32(ctx, addr, val);
            }
            ctx->regs.eip += 2 + m.len + 1;
            return true;
        }

        /* XORPS (0x0F 0x57) / ANDPS (0x54) / ORPS (0x56) */
        if (op2 == 0x57 || op2 == 0x54 || op2 == 0x56) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            for (int k = 0; k < 4; k++) {
                uint32_t v = 0;
                if (m.mod == 3) v = ctx->xmm[m.rm][k];
                else x86_mem_read32(ctx, modrm_effective_addr(&m, gprs) + k * 4, &v);
                if (op2 == 0x57) ctx->xmm[m.reg][k] ^= v;
                else if (op2 == 0x54) ctx->xmm[m.reg][k] &= v;
                else ctx->xmm[m.reg][k] |= v;
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVUPS / MOVAPS load (0x0F 0x10 / 0x0F 0x28) or MOVLPS (0x0F 0x12) */
        if (op2 == 0x10 || op2 == 0x28 || op2 == 0x12) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            int nw = (op2 == 0x12) ? 2 : 4;
            if (m.mod == 3) {
                for (int k = 0; k < nw; k++) ctx->xmm[m.reg][k] = ctx->xmm[m.rm][k];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                for (int k = 0; k < nw; k++) x86_mem_read32(ctx, addr + k * 4, &ctx->xmm[m.reg][k]);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        /* MOVUPS / MOVAPS store (0x0F 0x11 / 0x0F 0x29) or MOVLPS (0x0F 0x13) */
        if (op2 == 0x11 || op2 == 0x29 || op2 == 0x13) {
            modrm_t m;
            decode_modrm(code + 2, &m);
            int nw = (op2 == 0x13) ? 2 : 4;
            if (m.mod == 3) {
                for (int k = 0; k < nw; k++) ctx->xmm[m.rm][k] = ctx->xmm[m.reg][k];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                for (int k = 0; k < nw; k++) x86_mem_write32(ctx, addr + k * 4, ctx->xmm[m.reg][k]);
            }
            ctx->regs.eip += 2 + m.len;
            return true;
        }

        fprintf(stderr, "[Interpreter] Unhandled 2-byte opcode 0x0F 0x%02X at EIP=0x%08X\n", op2, (unsigned int)ctx->regs.eip);
        ctx->running = false;
        return false;
    }

    /* JMP rel8 */
    if (op == 0xEB) {
        int8_t disp = (int8_t)code[1];
        ctx->regs.eip += 2 + disp;
        return true;
    }

    /* JMP rel32 */
    if (op == 0xE9) {
        int32_t disp = (int32_t)((uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                                 ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24));
        ctx->regs.eip += 5 + disp;
        return true;
    }

    /* LEAVE (0xC9) -> mov esp, ebp; pop ebp */
    if (op == 0xC9) {
        ctx->regs.esp = ctx->regs.ebp;
        uint32_t old_ebp = 0;
        x86_mem_read32(ctx, ctx->regs.esp, &old_ebp);
        ctx->regs.esp += 4;
        ctx->regs.ebp = old_ebp;
        ctx->regs.eip += 1;
        return true;
    }

    /* RET */
    if (op == 0xC3) {
        uint32_t ret_addr = 0;
        x86_mem_read32(ctx, ctx->regs.esp, &ret_addr);
        ctx->regs.esp += 4;
        ctx->regs.eip = ret_addr;
        return true;
    }

    /* RET imm16 */
    if (op == 0xC2) {
        uint16_t imm = (uint16_t)code[1] | ((uint16_t)code[2] << 8);
        uint32_t ret_addr = 0;
        x86_mem_read32(ctx, ctx->regs.esp, &ret_addr);
        ctx->regs.esp += 4 + imm;
        ctx->regs.eip = ret_addr;
        return true;
    }

    /* CALL rel32 */
    if (op == 0xE8) {
        int32_t disp = (int32_t)((uint32_t)code[1] | ((uint32_t)code[2] << 8) |
                                 ((uint32_t)code[3] << 16) | ((uint32_t)code[4] << 24));
        uint32_t ret_va = ctx->regs.eip + 5;
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, ret_va);
        ctx->regs.eip = ret_va + disp;
        return true;
    }

    /* Group 4 (0xFE): INC r/m8 (/0), DEC r/m8 (/1) */
    if (op == 0xFE) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) { /* INC r/m8 */
            uint8_t val = 0;
            if (m.mod == 3) {
                val = ++((uint8_t *)&gprs[m.rm & 3])[m.rm >= 4 ? 1 : 0];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read8(ctx, addr, &val);
                val++;
                x86_mem_write8(ctx, addr, val);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
            if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (val & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val == 0x80) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 1) { /* DEC r/m8 */
            uint8_t val = 0;
            if (m.mod == 3) {
                val = --((uint8_t *)&gprs[m.rm & 3])[m.rm >= 4 ? 1 : 0];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read8(ctx, addr, &val);
                val--;
                x86_mem_write8(ctx, addr, val);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
            if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (val & 0x80) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val == 0x7F) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    /* Group 5 (0xFF): INC (/0), DEC (/1), CALL (/2), JMP (/4), PUSH (/6) */
    if (op == 0xFF) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) { /* INC r/m32 */
            uint32_t val = 0;
            if (m.mod == 3) {
                val = ++gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &val);
                val++;
                x86_mem_write32(ctx, addr, val);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
            if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val == 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 1) { /* DEC r/m32 */
            uint32_t val = 0;
            if (m.mod == 3) {
                val = --gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &val);
                val--;
                x86_mem_write32(ctx, addr, val);
            }
            ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
            if (val == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
            if (val & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
            if (val == 0x7FFFFFFF) ctx->regs.eflags |= X86_EFLAGS_OF;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 2) { /* CALL r/m32 */
            uint32_t target_func = 0;
            if (m.mod == 3) {
                target_func = gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &target_func);
            }
            uint32_t ret_va = ctx->regs.eip + 1 + m.len;
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, ret_va);
            ctx->regs.eip = target_func;
            return true;
        }
        if (m.reg == 4) { /* JMP r/m32 */
            uint32_t target_func = 0;
            if (m.mod == 3) {
                target_func = gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &target_func);
            }
            ctx->regs.eip = target_func;
            return true;
        }
        if (m.reg == 6) { /* PUSH r/m32 */
            uint32_t val = 0;
            if (m.mod == 3) {
                val = gprs[m.rm];
            } else {
                uint32_t addr = modrm_effective_addr(&m, gprs);
                x86_mem_read32(ctx, addr, &val);
            }
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, val);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    /* ====== x87 FPU Opcodes (0xD9, 0xDD) ====== */
    if (op == 0xD9) {
        uint8_t op2 = code[1];
        if (op2 == 0xE8) { /* fld1 */
            ctx->fpu_top = (ctx->fpu_top - 1) & 7;
            ctx->fpu_stack[ctx->fpu_top] = 1.0;
            ctx->regs.eip += 2;
            return true;
        }
        if (op2 == 0xEE) { /* fldz */
            ctx->fpu_top = (ctx->fpu_top - 1) & 7;
            ctx->fpu_stack[ctx->fpu_top] = 0.0;
            ctx->regs.eip += 2;
            return true;
        }
        if (op2 == 0xE0) { /* fchs */
            ctx->fpu_stack[ctx->fpu_top] = -ctx->fpu_stack[ctx->fpu_top];
            ctx->regs.eip += 2;
            return true;
        }
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) { /* flds m32 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint32_t raw = 0;
            x86_mem_read32(ctx, addr, &raw);
            float f;
            memcpy(&f, &raw, sizeof(f));
            ctx->fpu_top = (ctx->fpu_top - 1) & 7;
            ctx->fpu_stack[ctx->fpu_top] = (double)f;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 2) { /* fsts m32 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            float f = (float)ctx->fpu_stack[ctx->fpu_top];
            uint32_t raw;
            memcpy(&raw, &f, sizeof(raw));
            x86_mem_write32(ctx, addr, raw);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 3) { /* fstps m32 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            float f = (float)ctx->fpu_stack[ctx->fpu_top];
            ctx->fpu_top = (ctx->fpu_top + 1) & 7;
            uint32_t raw;
            memcpy(&raw, &f, sizeof(raw));
            x86_mem_write32(ctx, addr, raw);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    if (op == 0xDD) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        if (m.reg == 0) { /* fldl m64 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            uint64_t raw = 0;
            x86_mem_read64(ctx, addr, &raw);
            double d;
            memcpy(&d, &raw, sizeof(d));
            ctx->fpu_top = (ctx->fpu_top - 1) & 7;
            ctx->fpu_stack[ctx->fpu_top] = d;
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 2) { /* fstl m64 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            double d = ctx->fpu_stack[ctx->fpu_top];
            uint64_t raw;
            memcpy(&raw, &d, sizeof(raw));
            x86_mem_write64(ctx, addr, raw);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
        if (m.reg == 3) { /* fstpl m64 */
            uint32_t addr = modrm_effective_addr(&m, gprs);
            double d = ctx->fpu_stack[ctx->fpu_top];
            ctx->fpu_top = (ctx->fpu_top + 1) & 7;
            uint64_t raw;
            memcpy(&raw, &d, sizeof(raw));
            x86_mem_write64(ctx, addr, raw);
            ctx->regs.eip += 1 + m.len;
            return true;
        }
    }

    if (op == 0xDB) {
        uint8_t op2 = code[1];
        if (op2 == 0xE3) { /* fninit / finit */
            ctx->fpu_top = 0;
            for (int i = 0; i < 8; i++) ctx->fpu_stack[i] = 0.0;
            ctx->regs.eip += 2;
            return true;
        }
    }

    /* Group 3 r/m32 (0xF7) - TEST / NOT / NEG / MUL / IMUL / DIV / IDIV */
    if (op == 0xF7) {
        modrm_t m;
        decode_modrm(code + 1, &m);
        uint32_t val = 0;
        if (m.mod == 3) {
            val = gprs[m.rm];
        } else {
            uint32_t addr = modrm_effective_addr(&m, gprs);
            x86_mem_read32(ctx, addr, &val);
        }

        switch (m.reg) {
            case 0: { /* TEST r/m32, imm32 */
                uint32_t imm = (uint32_t)code[1 + m.len] | ((uint32_t)code[2 + m.len] << 8) |
                               ((uint32_t)code[3 + m.len] << 16) | ((uint32_t)code[4 + m.len] << 24);
                uint32_t res = val & imm;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                ctx->regs.eip += 1 + m.len + 4;
                return true;
            }
            case 2: { /* NOT r/m32 */
                val = ~val;
                if (m.mod == 3) gprs[m.rm] = val;
                else { uint32_t addr = modrm_effective_addr(&m, gprs); x86_mem_write32(ctx, addr, val); }
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            case 3: { /* NEG r/m32 */
                uint32_t res = 0 - val;
                ctx->regs.eflags &= ~(X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (res == 0) ctx->regs.eflags |= X86_EFLAGS_ZF;
                if (res & 0x80000000) ctx->regs.eflags |= X86_EFLAGS_SF;
                if (val != 0) ctx->regs.eflags |= X86_EFLAGS_CF;
                if (val == 0x80000000) ctx->regs.eflags |= X86_EFLAGS_OF;
                if (m.mod == 3) gprs[m.rm] = res;
                else { uint32_t addr = modrm_effective_addr(&m, gprs); x86_mem_write32(ctx, addr, res); }
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            case 4: { /* MUL r/m32: EDX:EAX = EAX * r/m32 */
                uint64_t prod = (uint64_t)ctx->regs.eax * (uint64_t)val;
                ctx->regs.eax = (uint32_t)prod;
                ctx->regs.edx = (uint32_t)(prod >> 32);
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (ctx->regs.edx != 0) ctx->regs.eflags |= (X86_EFLAGS_CF | X86_EFLAGS_OF);
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            case 5: { /* IMUL r/m32: EDX:EAX = EAX * r/m32 (signed) */
                int64_t prod = (int64_t)(int32_t)ctx->regs.eax * (int64_t)(int32_t)val;
                ctx->regs.eax = (uint32_t)prod;
                ctx->regs.edx = (uint32_t)(prod >> 32);
                ctx->regs.eflags &= ~(X86_EFLAGS_CF | X86_EFLAGS_OF);
                if (prod < -2147483648LL || prod > 2147483647LL)
                    ctx->regs.eflags |= (X86_EFLAGS_CF | X86_EFLAGS_OF);
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            case 6: { /* DIV r/m32: EDX:EAX / r/m32 (unsigned) */
                if (val == 0) {
                    fprintf(stderr, "[Interpreter] Divide by zero at EIP=0x%08X\n", (unsigned int)ctx->regs.eip);
                    ctx->running = false;
                    return false;
                }
                uint64_t dividend = ((uint64_t)ctx->regs.edx << 32) | (uint64_t)ctx->regs.eax;
                ctx->regs.eax = (uint32_t)(dividend / val);
                ctx->regs.edx = (uint32_t)(dividend % val);
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            case 7: { /* IDIV r/m32: EDX:EAX / r/m32 (signed) */
                int32_t divisor = (int32_t)val;
                if (divisor == 0) {
                    fprintf(stderr, "[Interpreter] Divide by zero at EIP=0x%08X\n", (unsigned int)ctx->regs.eip);
                    ctx->running = false;
                    return false;
                }
                int64_t dividend = (int64_t)(((uint64_t)ctx->regs.edx << 32) | (uint64_t)ctx->regs.eax);
                ctx->regs.eax = (uint32_t)(int32_t)(dividend / divisor);
                ctx->regs.edx = (uint32_t)(int32_t)(dividend % divisor);
                ctx->regs.eip += 1 + m.len;
                return true;
            }
            default: goto unhandled;
        }
    }

unhandled:
    fprintf(stderr, "[Interpreter] Unhandled opcode 0x%02X at EIP=0x%08X (ESP=0x%08X)\n", op, (unsigned int)ctx->regs.eip, (unsigned int)ctx->regs.esp);
    fprintf(stderr, "Recent EIPs:\n");
    for (int k = 0; k < 16; k++) {
        size_t idx = (s_last_eip_idx + 32 - 16 + k) % 32;
        fprintf(stderr, "  [%d] 0x%08X\n", k, (unsigned int)s_last_eips[idx]);
    }
    fprintf(stderr, "Stack contents:\n");
    for (int k = 0; k < 16; k++) {
        uint32_t val = 0;
        x86_mem_read32(ctx, ctx->regs.esp + k * 4, &val);
        fprintf(stderr, "  stack[%d] (ESP+%d) = 0x%08X\n", k, k * 4, (unsigned int)val);
    }
    ctx->running = false;
    return false;
}
