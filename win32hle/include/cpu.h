#ifndef WIN32HLE_CPU_H
#define WIN32HLE_CPU_H

#include "types.h"

typedef struct CPUState {
    union {
        uint32_t regs[8];
        struct {
            uint32_t eax;
            uint32_t ecx;
            uint32_t edx;
            uint32_t ebx;
            uint32_t esp;
            uint32_t ebp;
            uint32_t esi;
            uint32_t edi;
        };
    };
    uint32_t eip;
    uint32_t eflags;

    // Segment base registers
    uint32_t fs_base;

    bool halted;
    uint32_t exit_code;
    uint64_t instruction_count;
} CPUState;

void cpu_init(CPUState* cpu, DWORD entry_point, DWORD stack_top, DWORD fs_base);
int  cpu_step(CPUState* cpu);
int  cpu_run(CPUState* cpu, uint64_t max_instructions);
void cpu_halt(CPUState* cpu, uint32_t exit_code);
void cpu_set_edx(CPUState* cpu, uint32_t val);

#endif // WIN32HLE_CPU_H
