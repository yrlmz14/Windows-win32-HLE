#ifndef NUCLEO_X86_CORE_H
#define NUCLEO_X86_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X86_EFLAGS_CF (1U << 0)
#define X86_EFLAGS_PF (1U << 2)
#define X86_EFLAGS_AF (1U << 4)
#define X86_EFLAGS_ZF (1U << 6)
#define X86_EFLAGS_SF (1U << 7)
#define X86_EFLAGS_TF (1U << 8)
#define X86_EFLAGS_IF (1U << 9)
#define X86_EFLAGS_DF (1U << 10)
#define X86_EFLAGS_OF (1U << 11)

typedef struct {
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t eip, eflags;
    uint16_t cs, ds, es, ss, fs, gs;
} x86_regs_t;

struct x86_context_t;
typedef struct x86_context_t x86_context_t;

typedef uint8_t *(*x86_va_to_host_fn)(const x86_context_t *ctx, uint32_t va);
typedef bool (*x86_mem_write_fn)(x86_context_t *ctx, uint32_t va, const void *buf, size_t len);

struct x86_context_t {
    x86_regs_t regs;
    bool       running;
    bool       yielded;
    uint32_t   exit_code;
    uint64_t   instructions_run;
    uint32_t   tib_data[64];
    double     fpu_stack[8];
    uint8_t    fpu_top;
    uint32_t   xmm[8][4];
    uint32_t   history_eips[16];
    uint8_t    history_idx;
    void      *user_data;
    x86_va_to_host_fn va_to_host;
    x86_mem_write_fn  mem_write;
};

static inline uint8_t *x86_va_to_host(const x86_context_t *ctx, uint32_t va) {
    if (!ctx || !ctx->va_to_host) return (uint8_t *)0;
    return ctx->va_to_host(ctx, va);
}

static inline bool x86_mem_read8(const x86_context_t *ctx, uint32_t va, uint8_t *val) {
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) { if (val) *val = 0; return false; }
    if (val) *val = *p;
    return true;
}

static inline bool x86_mem_write8(x86_context_t *ctx, uint32_t va, uint8_t val) {
    if (ctx->mem_write) return ctx->mem_write(ctx, va, &val, 1);
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) return false;
    *p = val;
    return true;
}

static inline bool x86_mem_read16(const x86_context_t *ctx, uint32_t va, uint16_t *val) {
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) { if (val) *val = 0; return false; }
    if (val) *val = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return true;
}

static inline bool x86_mem_write16(x86_context_t *ctx, uint32_t va, uint16_t val) {
    uint8_t b[2] = { (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF) };
    if (ctx->mem_write) return ctx->mem_write(ctx, va, b, 2);
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) return false;
    p[0] = b[0]; p[1] = b[1];
    return true;
}

static inline bool x86_mem_read32(const x86_context_t *ctx, uint32_t va, uint32_t *val) {
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) { if (val) *val = 0; return false; }
    if (val) *val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

static inline bool x86_mem_write32(x86_context_t *ctx, uint32_t va, uint32_t val) {
    uint8_t b[4] = { (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF),
                     (uint8_t)((val >> 16) & 0xFF), (uint8_t)((val >> 24) & 0xFF) };
    if (ctx->mem_write) return ctx->mem_write(ctx, va, b, 4);
    uint8_t *p = x86_va_to_host(ctx, va);
    if (!p) return false;
    p[0] = b[0]; p[1] = b[1]; p[2] = b[2]; p[3] = b[3];
    return true;
}

static inline bool x86_mem_read64(const x86_context_t *ctx, uint32_t va, uint64_t *val) {
    uint32_t lo = 0, hi = 0;
    if (!x86_mem_read32(ctx, va, &lo) || !x86_mem_read32(ctx, va + 4, &hi)) return false;
    if (val) *val = (uint64_t)lo | ((uint64_t)hi << 32);
    return true;
}

static inline bool x86_mem_write64(x86_context_t *ctx, uint32_t va, uint64_t val) {
    return x86_mem_write32(ctx, va, (uint32_t)val) && x86_mem_write32(ctx, va + 4, (uint32_t)(val >> 32));
}

bool x86_interpret_instruction(x86_context_t *ctx);

static inline bool x86_step(x86_context_t *ctx) {
    return x86_interpret_instruction(ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* NUCLEO_X86_CORE_H */
