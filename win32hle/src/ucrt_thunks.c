#include "hle.h"
#include "memory.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_ucrt_iob_base = 0;

static uint32_t hle_ucrt_iob_func(struct CPUState* cpu, uint32_t* args) {
    uint32_t index = args[0];
    if (!s_ucrt_iob_base) {
        s_ucrt_iob_base = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    return s_ucrt_iob_base + (index * 32);
}

static uint32_t hle_ucrt_initialize_narrow_environment(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_ucrt_configure_narrow_argv(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_ucrt_get_initial_narrow_environment(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_ucrt_initterm(struct CPUState* cpu, uint32_t* args) {
    DWORD first = args[0];
    DWORD last = args[1];

    if (!first || !last) return 0;

    for (DWORD p = first; p < last; p += 4) {
        DWORD func = mem_read_u32(p);
        if (func != 0) {
            // Function pointer to call
            // In a full HLE, CPU can execute this constructor
        }
    }
    return 0;
}

static uint32_t hle_ucrt_stdio_common_vfprintf(struct CPUState* cpu, uint32_t* args) {
    // uint64_t options, FILE* stream, const char* format, _locale_t locale, va_list valist
    // In 32-bit: options (args[0], args[1]), stream (args[2]), format (args[3]), locale (args[4]), valist (args[5])
    DWORD stream_ptr = args[2];
    DWORD format_ptr = args[3];

    const char* fmt = (const char*)mem_to_host(format_ptr);
    if (!fmt) return 0;

    int fd = 1;
    if (stream_ptr == s_ucrt_iob_base + 64) fd = 2; // stderr

    // Direct output format string
    fprintf(fd == 2 ? stderr : stdout, "%s", fmt);
    fflush(fd == 2 ? stderr : stdout);
    return (uint32_t)strlen(fmt);
}

static uint32_t hle_ucrt_malloc(struct CPUState* cpu, uint32_t* args) {
    SIZE_T size = args[0];
    return mem_virtual_alloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static uint32_t hle_ucrt_free(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    if (ptr) mem_virtual_free(ptr, 0, MEM_RELEASE);
    return 0;
}

static uint32_t s_wide_cmd_line = 0;
static uint32_t s_commode = 0;
static uint32_t s_errno_box = 0;

static uint32_t hle_ucrt_configure_wide_argv(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_initialize_onexit_table(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_register_onexit_function(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_seh_filter_exe(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_set_app_type(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_get_wide_winmain_command_line(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    if (!s_wide_cmd_line) {
        s_wide_cmd_line = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        mem_write_u16(s_wide_cmd_line, 0); // L""
    }
    return s_wide_cmd_line;
}

static uint32_t hle_ucrt_initialize_wide_environment(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_cexit(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_exit(struct CPUState* cpu, uint32_t* args) {
    cpu->halted = true;
    cpu->exit_code = args[0];
    return 0;
}

static uint32_t hle_ucrt_p__commode(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    if (!s_commode) {
        s_commode = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        mem_write_u32(s_commode, 0);
    }
    return s_commode;
}

static uint32_t hle_ucrt_errno(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    if (!s_errno_box) {
        s_errno_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        mem_write_u32(s_errno_box, 0);
    }
    return s_errno_box;
}

static uint32_t hle_ucrt_controlfp_s(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t current_ctrl = args[0];
    if (current_ctrl) mem_write_u32(current_ctrl, 0x0008001F);
    return 0;
}

static uint32_t hle_ucrt_configthreadlocale(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_set_fmode(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_set_new_mode(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_wcsnlen(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t str_ptr = args[0];
    uint32_t maxlen = args[1];
    if (!str_ptr) return 0;
    const uint16_t* s = (const uint16_t*)mem_to_host(str_ptr);
    uint32_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

static uint32_t hle_ucrt_register_thread_local(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_ucrt_memcpy(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    void* dst = mem_to_host(args[0]);
    void* src = mem_to_host(args[1]);
    if (dst && src && args[2]) memcpy(dst, src, args[2]);
    return args[0];
}

static uint32_t hle_ucrt_memmove(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    void* dst = mem_to_host(args[0]);
    void* src = mem_to_host(args[1]);
    if (dst && src && args[2]) memmove(dst, src, args[2]);
    return args[0];
}

static uint32_t hle_ucrt_memset(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    void* dst = mem_to_host(args[0]);
    if (dst && args[2]) memset(dst, (int)args[1], args[2]);
    return args[0];
}

static uint32_t hle_ucrt_memcmp(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    void* p1 = mem_to_host(args[0]);
    void* p2 = mem_to_host(args[1]);
    if (!p1 || !p2) return 0;
    return (uint32_t)memcmp(p1, p2, args[2]);
}

void register_ucrt_apis(void) {
    hle_register("ucrtbase.dll", "__acrt_iob_func", 0, hle_ucrt_iob_func, 1, true);
    hle_register("ucrtbase.dll", "_initialize_narrow_environment", 0, hle_ucrt_initialize_narrow_environment, 0, true);
    hle_register("ucrtbase.dll", "_configure_narrow_argv", 0, hle_ucrt_configure_narrow_argv, 1, true);
    hle_register("ucrtbase.dll", "_get_initial_narrow_environment", 0, hle_ucrt_get_initial_narrow_environment, 0, true);
    hle_register("ucrtbase.dll", "_initterm", 0, hle_ucrt_initterm, 2, true);
    hle_register("ucrtbase.dll", "_initterm_e", 0, hle_ucrt_initterm, 2, true);
    hle_register("ucrtbase.dll", "__stdio_common_vfprintf", 0, hle_ucrt_stdio_common_vfprintf, 6, true);
    hle_register("ucrtbase.dll", "malloc", 0, hle_ucrt_malloc, 1, true);
    hle_register("ucrtbase.dll", "free", 0, hle_ucrt_free, 1, true);

    // Modern UCRT (Windows 10 / CRT Private _o_* mappings)
    hle_register("ucrtbase.dll", "_configure_wide_argv", 0, hle_ucrt_configure_wide_argv, 1, true);
    hle_register("ucrtbase.dll", "_o__configure_wide_argv", 0, hle_ucrt_configure_wide_argv, 1, true);
    hle_register("ucrtbase.dll", "_initialize_onexit_table", 0, hle_ucrt_initialize_onexit_table, 1, true);
    hle_register("ucrtbase.dll", "_o__initialize_onexit_table", 0, hle_ucrt_initialize_onexit_table, 1, true);
    hle_register("ucrtbase.dll", "_register_onexit_function", 0, hle_ucrt_register_onexit_function, 2, true);
    hle_register("ucrtbase.dll", "_o__register_onexit_function", 0, hle_ucrt_register_onexit_function, 2, true);
    hle_register("ucrtbase.dll", "_seh_filter_exe", 0, hle_ucrt_seh_filter_exe, 2, true);
    hle_register("ucrtbase.dll", "_o__seh_filter_exe", 0, hle_ucrt_seh_filter_exe, 2, true);
    hle_register("ucrtbase.dll", "_set_app_type", 0, hle_ucrt_set_app_type, 1, true);
    hle_register("ucrtbase.dll", "_o__set_app_type", 0, hle_ucrt_set_app_type, 1, true);
    hle_register("ucrtbase.dll", "_get_wide_winmain_command_line", 0, hle_ucrt_get_wide_winmain_command_line, 0, true);
    hle_register("ucrtbase.dll", "_o__get_wide_winmain_command_line", 0, hle_ucrt_get_wide_winmain_command_line, 0, true);
    hle_register("ucrtbase.dll", "_initialize_wide_environment", 0, hle_ucrt_initialize_wide_environment, 0, true);
    hle_register("ucrtbase.dll", "_o__initialize_wide_environment", 0, hle_ucrt_initialize_wide_environment, 0, true);
    hle_register("ucrtbase.dll", "_cexit", 0, hle_ucrt_cexit, 0, true);
    hle_register("ucrtbase.dll", "_o__cexit", 0, hle_ucrt_cexit, 0, true);
    hle_register("ucrtbase.dll", "_c_exit", 0, hle_ucrt_cexit, 0, true);
    hle_register("ucrtbase.dll", "exit", 0, hle_ucrt_exit, 1, true);
    hle_register("ucrtbase.dll", "_o_exit", 0, hle_ucrt_exit, 1, true);
    hle_register("ucrtbase.dll", "_exit", 0, hle_ucrt_exit, 1, true);
    hle_register("ucrtbase.dll", "_o__exit", 0, hle_ucrt_exit, 1, true);
    hle_register("ucrtbase.dll", "_o_malloc", 0, hle_ucrt_malloc, 1, true);
    hle_register("ucrtbase.dll", "_o_free", 0, hle_ucrt_free, 1, true);
    hle_register("ucrtbase.dll", "__p__commode", 0, hle_ucrt_p__commode, 0, true);
    hle_register("ucrtbase.dll", "_o___p__commode", 0, hle_ucrt_p__commode, 0, true);
    hle_register("ucrtbase.dll", "_errno", 0, hle_ucrt_errno, 0, true);
    hle_register("ucrtbase.dll", "_o__errno", 0, hle_ucrt_errno, 0, true);
    hle_register("ucrtbase.dll", "_controlfp_s", 0, hle_ucrt_controlfp_s, 3, true);
    hle_register("ucrtbase.dll", "_o__controlfp_s", 0, hle_ucrt_controlfp_s, 3, true);
    hle_register("ucrtbase.dll", "_configthreadlocale", 0, hle_ucrt_configthreadlocale, 1, true);
    hle_register("ucrtbase.dll", "_o__configthreadlocale", 0, hle_ucrt_configthreadlocale, 1, true);
    hle_register("ucrtbase.dll", "_set_fmode", 0, hle_ucrt_set_fmode, 1, true);
    hle_register("ucrtbase.dll", "_o__set_fmode", 0, hle_ucrt_set_fmode, 1, true);
    hle_register("ucrtbase.dll", "_set_new_mode", 0, hle_ucrt_set_new_mode, 1, true);
    hle_register("ucrtbase.dll", "_o__set_new_mode", 0, hle_ucrt_set_new_mode, 1, true);
    hle_register("ucrtbase.dll", "wcsnlen", 0, hle_ucrt_wcsnlen, 2, true);
    hle_register("ucrtbase.dll", "_register_thread_local_exe_atexit_callback", 0, hle_ucrt_register_thread_local, 1, true);
    hle_register("ucrtbase.dll", "memcpy", 0, hle_ucrt_memcpy, 3, true);
    hle_register("ucrtbase.dll", "memmove", 0, hle_ucrt_memmove, 3, true);
    hle_register("ucrtbase.dll", "memset", 0, hle_ucrt_memset, 3, true);
    hle_register("ucrtbase.dll", "memcmp", 0, hle_ucrt_memcmp, 3, true);
}
