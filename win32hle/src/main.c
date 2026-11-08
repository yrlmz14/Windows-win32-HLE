#include "types.h"
#include "memory.h"
#include "teb_peb.h"
#include "apiset.h"
#include "hle.h"
#include "sync.h"
#include "pe_loader.h"
#include "cpu.h"
#include "arm64_dynarec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void register_kernel32_base_apis(void);
extern void register_win7_10_syscalls(void);
extern void register_ucrt_apis(void);
extern void register_msvcrt_apis(void);
extern void msvcrt_init_cmdline(int argc, char** argv);

static void print_banner(void) {
    printf("=========================================================\n");
    printf(" Win32 HLE Runtime (Windows 7 / Windows 10 Target)\n");
    printf(" Hardware Target: ARM64 (AArch64) | 512MB RAM Budget\n");
    printf(" Binary Emulator: Dual-Engine ARM64 Dynarec + Fallback\n");
    printf("=========================================================\n");
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options] <executable.exe> [app args...]\n", prog);
    printf("Options:\n");
    printf("  --winver <7|10>      Emulate Windows 7 or Windows 10 environment (default: 10)\n");
    printf("  --ram-limit <MB>     Maximum committed memory limit in MB (default: 384 for 512MB RAM)\n");
    printf("  --trace              Trace all Win32 API calls\n");
    printf("  --jit                Enable ARM64 binary dynarec / JIT compiler\n");
    printf("  --dump-jit           Dump compiled native ARM64 assembly words\n");
    printf("  --help               Display this help message\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_banner();
        print_usage(argv[0]);
        return 1;
    }

    const char* exe_path = NULL;
    WindowsVersionProfile winver = WIN_VER_10;
    size_t ram_limit_mb = 384;
    bool trace = false;
    bool enable_jit = false;
    bool dump_jit = false;

    int app_arg_start = argc;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_banner();
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--winver") == 0 && i + 1 < argc) {
            int ver = atoi(argv[++i]);
            if (ver == 7) winver = WIN_VER_7;
            else if (ver == 10) winver = WIN_VER_10;
        } else if (strcmp(argv[i], "--ram-limit") == 0 && i + 1 < argc) {
            ram_limit_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--jit") == 0) {
            enable_jit = true;
        } else if (strcmp(argv[i], "--dump-jit") == 0) {
            enable_jit = true;
            dump_jit = true;
        } else if (!exe_path) {
            exe_path = argv[i];
            app_arg_start = i;
            break;
        }
    }

    if (!exe_path) {
        fprintf(stderr, "Error: No executable provided.\n");
        return 1;
    }

    print_banner();
    printf("[HLE] Profile: Windows %s | Memory Budget: %zu MB committed max\n",
           (winver == WIN_VER_10) ? "10 (Build 19045)" : "7 SP1 (Build 7601)",
           ram_limit_mb);
    printf("[HLE] ARM64 Binary Emulator: %s\n", enable_jit ? "Active (Dynarec + Fallback)" : "Interpreter");

    // 1. Initialize Virtual Memory Manager
    size_t max_commit_bytes = ram_limit_mb * 1024 * 1024;
    if (mem_init(max_commit_bytes) != 0) {
        fprintf(stderr, "Error: Failed to initialize memory manager.\n");
        return 1;
    }

    // 2. Initialize ARM64 Dynarec Cache
    if (enable_jit) {
        arm64_dynarec_init(4 * 1024 * 1024, dump_jit);
    }

    // 2. Initialize HLE dispatch table and handle subsystem
    hle_init(trace);

    // 3. Register standard and modern APIs
    register_kernel32_base_apis();
    register_win7_10_syscalls();
    register_ucrt_apis();
    register_msvcrt_apis();
    extern void register_user32_apis(void);
    register_user32_apis();

    int app_argc = argc - app_arg_start;
    char** app_argv = &argv[app_arg_start];
    msvcrt_init_cmdline(app_argc, app_argv);
    extern void kernel32_set_command_line(int argc, char** argv);
    kernel32_set_command_line(app_argc, app_argv);

    // 4. Allocate 1MB stack space (from 0x000F0000 to 0x001F0000)
    DWORD stack_base = 0x00100000;
    DWORD stack_size = 0x000F0000; // ~1MB
    mem_virtual_alloc(stack_base - stack_size, stack_size + PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD stack_top = stack_base - 16; // 16-byte aligned top

    // 5. Load PE executable
    LoadedPE pe;
    if (pe_load(exe_path, &pe) != 0) {
        fprintf(stderr, "Error: Failed to load PE binary '%s'\n", exe_path);
        mem_cleanup();
        return 1;
    }

    // 6. Setup TEB, PEB, KUSER_SHARED_DATA
    init_teb_peb_shared_data(winver, pe.image_base);

    printf("[HLE] Loaded PE: Base=0x%08X, EntryPoint=0x%08X, Subsystem=%u\n",
           pe.image_base, pe.entry_point, pe.subsystem);
    printf("[HLE] Starting execution...\n");
    printf("---------------------------------------------------------\n");

    // 7. Initialize CPU and run
    CPUState cpu;
    cpu_init(&cpu, pe.entry_point, stack_top, DEFAULT_TEB_ADDRESS);

    int exit_code = cpu_run(&cpu, 0);

    printf("\n---------------------------------------------------------\n");
    printf("[HLE] Execution finished. Exit Code: %d (0x%X)\n", exit_code, exit_code);
    printf("[HLE] Instructions executed: %llu\n", (unsigned long long)cpu.instruction_count);
    printf("[HLE] Peak committed memory: %zu KB / %zu KB budget\n",
           g_mem.current_committed / 1024, max_commit_bytes / 1024);

    mem_cleanup();
    return exit_code;
}
