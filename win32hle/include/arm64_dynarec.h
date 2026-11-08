#ifndef WIN32HLE_ARM64_DYNAREC_H
#define WIN32HLE_ARM64_DYNAREC_H

#include "types.h"
#include "cpu.h"
#include <stdint.h>
#include <stdbool.h>

// -------------------------------------------------------------
// ARM64 Dynarec Register Allocations
// -------------------------------------------------------------
// x86 Guest Registers are pinned to callee-saved ARM64 registers:
//   x86 EAX  <-> ARM64 W19
//   x86 ECX  <-> ARM64 W20
//   x86 EDX  <-> ARM64 W21
//   x86 EBX  <-> ARM64 W22
//   x86 ESP  <-> ARM64 W23
//   x86 EBP  <-> ARM64 W24
//   x86 ESI  <-> ARM64 W25
//   x86 EDI  <-> ARM64 W26
//
// Special Base Registers:
//   Win32 2GB Virtual Arena Base Address <-> ARM64 X28
//   TEB (FS Segment Base)                <-> ARM64 X27
//
// Scratch Registers for JIT translation:
//   W0..W7, W8..W15 (temporaries & C ABI arguments)
// -------------------------------------------------------------

#define ARM64_REG_EAX 19
#define ARM64_REG_ECX 20
#define ARM64_REG_EDX 21
#define ARM64_REG_EBX 22
#define ARM64_REG_ESP 23
#define ARM64_REG_EBP 24
#define ARM64_REG_ESI 25
#define ARM64_REG_EDI 26

#define ARM64_REG_FS_BASE 27
#define ARM64_REG_MEM_BASE 28

#define ARM64_REG_TMP0 8
#define ARM64_REG_TMP1 9
#define ARM64_REG_TMP2 10

// Default JIT Code Cache: 4MB (fits well within 512MB RAM target hardware)
#define DEFAULT_JIT_CACHE_SIZE (4 * 1024 * 1024)
#define JIT_MAX_BLOCKS 65536

typedef struct TranslationBlock {
    uint32_t x86_entry;     // Guest x86 start address
    uint32_t x86_size;      // Guest byte size
    uint32_t arm64_offset;  // Byte offset inside JIT code buffer
    uint32_t arm64_size;    // Size of generated A64 instructions in bytes
    uint32_t exec_count;    // Execution frequency
} TranslationBlock;

typedef struct {
    uint8_t* code_buffer;       // Executable mmap buffer for native A64 code
    size_t   code_capacity;     // Total bytes allocated (e.g. 4MB)
    size_t   code_size;         // Currently used code bytes
    TranslationBlock* blocks;   // Translation block table
    uint32_t block_count;
    bool     enabled;           // True if running on ARM64 host with Dynarec active
    bool     dump_jit;          // Debug flag: print emitted ARM64 machine words
} ARM64Dynarec;

extern ARM64Dynarec g_dynarec;

int  arm64_dynarec_init(size_t cache_size, bool dump_jit);
void arm64_dynarec_cleanup(void);

// Compile an x86 basic block into native ARM64 machine code
TranslationBlock* arm64_dynarec_compile(CPUState* cpu, uint32_t x86_pc);

// Find existing compiled block
TranslationBlock* arm64_dynarec_find(uint32_t x86_pc);

// Run the translated block natively on ARM64
int arm64_dynarec_exec(CPUState* cpu, TranslationBlock* tb);

#endif // WIN32HLE_ARM64_DYNAREC_H
