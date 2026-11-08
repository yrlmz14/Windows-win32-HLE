#include "msvcrt.h"
#include "kernel32.h"
#include "win32_dispatch.h"
#include <time.h>
#include <ctype.h>

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

/* Static storage in guest memory for CRT global variables */
static uint32_t s_fmode_va = 0;
static uint32_t s_commode_va = 0;
static uint32_t s_acmdln_va = 0;

static uint32_t crt_set_app_type(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    LOGI("msvcrt!__set_app_type(%u)", args[0]);
    return 0;
}

static uint32_t crt_p_fmode(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_fmode_va) {
        s_fmode_va = 0x003F0000;
        guest_mem_commit(&proc->mem, s_fmode_va, 4096);
        uint32_t val = 0;
        guest_mem_write(&proc->mem, s_fmode_va, &val, 4);
    }
    return s_fmode_va;
}

static uint32_t crt_p_commode(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_commode_va) {
        s_commode_va = 0x003F1000;
        guest_mem_commit(&proc->mem, s_commode_va, 4096);
        uint32_t val = 0;
        guest_mem_write(&proc->mem, s_commode_va, &val, 4);
    }
    return s_commode_va;
}

static uint32_t crt_controlfp(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x0008001F; /* Standard default x87 control word */
}

static uint32_t crt_time(win32_process_t *proc, uint32_t *args) {
    uint32_t timer_va = args[0];
    uint32_t t = (uint32_t)time(NULL);
    if (timer_va) {
        guest_mem_write(&proc->mem, timer_va, &t, 4);
    }
    return t;
}

static uint32_t crt_initterm(win32_process_t *proc, uint32_t *args) {
    uint32_t start_va = args[0];
    uint32_t end_va = args[1];

    LOGI("msvcrt!_initterm(0x%08X, 0x%08X)", start_va, end_va);
    for (uint32_t va = start_va; va < end_va; va += 4) {
        uint32_t fn_ptr = 0;
        if (guest_mem_read(&proc->mem, va, &fn_ptr, 4) && fn_ptr != 0) {
            LOGI("msvcrt!_initterm calling initializer at 0x%08X", fn_ptr);
            /* Initializers are usually C++ static ctors. Can be invoked if needed */
        }
    }
    return 0;
}

static uint32_t crt_getmainargs(win32_process_t *proc, uint32_t *args) {
    uint32_t argc_va = args[0];
    uint32_t argv_va = args[1];
    uint32_t env_va  = args[2];

    LOGI("msvcrt!__getmainargs(argc_va=0x%08X, argv_va=0x%08X)", argc_va, argv_va);

    /* Allocate dummy argv array in guest memory */
    uint32_t dummy_argv_va = 0x003F2000;
    guest_mem_commit(&proc->mem, dummy_argv_va, 4096);

    uint32_t app_name_va = dummy_argv_va + 64;
    guest_mem_write(&proc->mem, app_name_va, "winmine.exe", 12);

    uint32_t argv_ptrs[2] = { app_name_va, 0 };
    guest_mem_write(&proc->mem, dummy_argv_va, argv_ptrs, sizeof(argv_ptrs));

    uint32_t one = 1;
    if (argc_va) guest_mem_write(&proc->mem, argc_va, &one, 4);
    if (argv_va) guest_mem_write(&proc->mem, argv_va, &dummy_argv_va, 4);
    if (env_va) {
        uint32_t null_ptr = dummy_argv_va + 32;
        uint32_t zero = 0;
        guest_mem_write(&proc->mem, null_ptr, &zero, 4);
        guest_mem_write(&proc->mem, env_va, &null_ptr, 4);
    }

    return 0;
}

static uint32_t crt_acmdln(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_acmdln_va) {
        s_acmdln_va = 0x003F3000;
        guest_mem_commit(&proc->mem, s_acmdln_va, 4096);
        uint32_t str_va = s_acmdln_va + 16;
        guest_mem_write(&proc->mem, str_va, "winmine.exe", 12);
        guest_mem_write(&proc->mem, s_acmdln_va, &str_va, 4);
    }
    return s_acmdln_va;
}

static uint32_t crt_srand(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t seed = args[0];
    LOGI("msvcrt!srand(%u)", seed);
    srand(seed);
    return 0;
}

static uint32_t crt_rand(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    int r = rand() & 0x7FFF;
    return (uint32_t)r;
}

static uint32_t crt_exit(win32_process_t *proc, uint32_t *args) {
    uint32_t code = args[0];
    LOGI("msvcrt!exit(%u)", code);
    proc->cpu.running = false;
    proc->cpu.exit_code = code;
    return 0;
}

static uint32_t crt_dummy_zero(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

/* Heap */
static uint32_t crt_malloc(win32_process_t *proc, uint32_t *args) {
    uint32_t bytes = args[0];
    uint32_t heap_args[3] = { 0, 0, bytes };
    return k32_HeapAlloc(proc, heap_args);
}
static uint32_t crt_free(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t crt_calloc(win32_process_t *proc, uint32_t *args) {
    uint32_t num = args[0];
    uint32_t size = args[1];
    uint32_t bytes = num * size;
    uint32_t heap_args[3] = { 0, 0x00000008, bytes };
    return k32_HeapAlloc(proc, heap_args);
}
static uint32_t crt_realloc(win32_process_t *proc, uint32_t *args) {
    uint32_t old_va = args[0];
    uint32_t bytes = args[1];
    uint32_t heap_args[3] = { 0, 0, bytes };
    uint32_t new_va = k32_HeapAlloc(proc, heap_args);
    if (new_va && old_va) {
        uint8_t *old_p = guest_va_to_host(&proc->mem, old_va);
        uint8_t *new_p = guest_va_to_host(&proc->mem, new_va);
        if (old_p && new_p) memcpy(new_p, old_p, bytes);
    }
    return new_va;
}

/* Strings & Memory */
static uint32_t crt_strlen(win32_process_t *proc, uint32_t *args) {
    const char *s = (const char *)guest_va_to_host(&proc->mem, args[0]);
    return s ? (uint32_t)strlen(s) : 0;
}
static uint32_t crt_wcslen(win32_process_t *proc, uint32_t *args) {
    const uint16_t *s = (const uint16_t *)guest_va_to_host(&proc->mem, args[0]);
    if (!s) return 0;
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}
static uint32_t crt_strcmp(win32_process_t *proc, uint32_t *args) {
    const char *s1 = (const char *)guest_va_to_host(&proc->mem, args[0]);
    const char *s2 = (const char *)guest_va_to_host(&proc->mem, args[1]);
    return (s1 && s2) ? (uint32_t)strcmp(s1, s2) : 0;
}
static uint32_t crt_strncmp(win32_process_t *proc, uint32_t *args) {
    const char *s1 = (const char *)guest_va_to_host(&proc->mem, args[0]);
    const char *s2 = (const char *)guest_va_to_host(&proc->mem, args[1]);
    size_t n = (size_t)args[2];
    return (s1 && s2) ? (uint32_t)strncmp(s1, s2, n) : 0;
}
static uint32_t crt_strcpy(win32_process_t *proc, uint32_t *args) {
    char *dst = (char *)guest_va_to_host(&proc->mem, args[0]);
    const char *src = (const char *)guest_va_to_host(&proc->mem, args[1]);
    if (dst && src) strcpy(dst, src);
    return args[0];
}
static uint32_t crt_strchr(win32_process_t *proc, uint32_t *args) {
    const char *s = (const char *)guest_va_to_host(&proc->mem, args[0]);
    int c = (int)args[1];
    if (!s) return 0;
    const char *p = strchr(s, c);
    return p ? (args[0] + (uint32_t)(p - s)) : 0;
}
static uint32_t crt_memset(win32_process_t *proc, uint32_t *args) {
    void *dst = guest_va_to_host(&proc->mem, args[0]);
    int val = (int)args[1];
    size_t count = (size_t)args[2];
    if (dst) memset(dst, val, count);
    return args[0];
}
static uint32_t crt_memcpy(win32_process_t *proc, uint32_t *args) {
    void *dst = guest_va_to_host(&proc->mem, args[0]);
    const void *src = guest_va_to_host(&proc->mem, args[1]);
    size_t count = (size_t)args[2];
    if (dst && src) memcpy(dst, src, count);
    return args[0];
}
static uint32_t crt_atoi(win32_process_t *proc, uint32_t *args) {
    const char *s = (const char *)guest_va_to_host(&proc->mem, args[0]);
    return s ? (uint32_t)atoi(s) : 0;
}
static uint32_t crt_strtol(win32_process_t *proc, uint32_t *args) {
    const char *s = (const char *)guest_va_to_host(&proc->mem, args[0]);
    int base = (int)args[2];
    char *end = NULL;
    long val = s ? strtol(s, &end, base) : 0;
    if (args[1] && end && s) {
        uint32_t end_va = args[0] + (uint32_t)(end - s);
        guest_mem_write(&proc->mem, args[1], &end_va, 4);
    }
    return (uint32_t)val;
}
static uint32_t crt_strtoul(win32_process_t *proc, uint32_t *args) {
    const char *s = (const char *)guest_va_to_host(&proc->mem, args[0]);
    int base = (int)args[2];
    char *end = NULL;
    unsigned long val = s ? strtoul(s, &end, base) : 0;
    if (args[1] && end && s) {
        uint32_t end_va = args[0] + (uint32_t)(end - s);
        guest_mem_write(&proc->mem, args[1], &end_va, 4);
    }
    return (uint32_t)val;
}
static uint32_t crt_tolower(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return (uint32_t)tolower((int)args[0]);
}
static uint32_t crt_isspace(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return (uint32_t)(isspace((int)args[0]) != 0);
}
static uint32_t crt_isxdigit(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return (uint32_t)(isxdigit((int)args[0]) != 0);
}
static uint32_t crt_strerror(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    static uint32_t s_err_va = 0;
    if (!s_err_va) {
        s_err_va = 0x003F6000;
        guest_mem_commit(&proc->mem, s_err_va, 4096);
        guest_mem_write(&proc->mem, s_err_va, "Unknown error", 14);
    }
    return s_err_va;
}

/* File I/O */
static uint32_t crt_fprintf(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    const char *fmt = (const char *)guest_va_to_host(&proc->mem, args[1]);
    if (fmt) LOGI("CRT fprintf: %s", fmt);
    return 0;
}
static uint32_t crt_fputc(win32_process_t *proc, uint32_t *args) {
    uint32_t ch = args[0];
    uint32_t stream_va = args[1];
    if (stream_va) {
        char *str = (char *)guest_va_to_host(&proc->mem, stream_va);
        if (str) {
            size_t len = strlen(str);
            if (len < 1024) {
                str[len] = (char)ch;
                str[len + 1] = '\0';
            }
        }
    }
    return ch;
}
static uint32_t crt_fwrite(win32_process_t *proc, uint32_t *args) {
    uint32_t ptr_va = args[0];
    uint32_t size = args[1];
    uint32_t count = args[2];
    uint32_t stream_va = args[3];
    uint32_t total = size * count;
    if (stream_va && total > 0) {
        uint32_t out_ptr = 0;
        if (guest_mem_read(&proc->mem, stream_va, &out_ptr, 4) && out_ptr) {
            for (uint32_t i = 0; i < total; i++) {
                uint8_t b = 0;
                guest_mem_read(&proc->mem, ptr_va + i, &b, 1);
                guest_mem_write(&proc->mem, out_ptr + i, &b, 1);
            }
            out_ptr += total;
            guest_mem_write(&proc->mem, stream_va, &out_ptr, 4);
            uint8_t zero = 0;
            guest_mem_write(&proc->mem, out_ptr, &zero, 1);
        }
    }
    return count;
}
static uint32_t crt_getc(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return (uint32_t)-1; /* EOF */
}
static uint32_t crt_ungetc(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}
static uint32_t crt_vfprintf(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

/* Environment & Globals */
static uint32_t s_initenv_va = 0;
static uint32_t crt_initenv(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_initenv_va) {
        s_initenv_va = 0x003F7000;
        guest_mem_commit(&proc->mem, s_initenv_va, 4096);
        uint32_t zero = 0;
        guest_mem_write(&proc->mem, s_initenv_va, &zero, 4);
    }
    return s_initenv_va;
}
static uint32_t s_errno_va = 0;
static uint32_t crt_errno(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_errno_va) {
        s_errno_va = 0x003F8000;
        guest_mem_commit(&proc->mem, s_errno_va, 4096);
        uint32_t zero = 0;
        guest_mem_write(&proc->mem, s_errno_va, &zero, 4);
    }
    return s_errno_va;
}
static uint32_t s_iob_va = 0;
static uint32_t crt_iob(win32_process_t *proc, uint32_t *args) {
    (void)args;
    if (!s_iob_va) {
        s_iob_va = 0x003F9000;
        guest_mem_commit(&proc->mem, s_iob_va, 4096);
    }
    return s_iob_va;
}
static uint32_t crt_mb_cur_max(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t crt_ismbblead(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t crt_localeconv(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    static uint32_t s_loc_va = 0;
    if (!s_loc_va) {
        s_loc_va = 0x003FA000;
        guest_mem_commit(&proc->mem, s_loc_va, 4096);
    }
    return s_loc_va;
}
static uint32_t crt_setlocale(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    static uint32_t s_c_str = 0;
    if (!s_c_str) {
        s_c_str = 0x003FB000;
        guest_mem_commit(&proc->mem, s_c_str, 4096);
        guest_mem_write(&proc->mem, s_c_str, "C", 2);
    }
    return s_c_str;
}
static uint32_t crt_signal(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t crt_onexit(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}
static uint32_t crt_amsg_exit(win32_process_t *proc, uint32_t *args) {
    LOGE("CRT _amsg_exit(%u)", args[0]);
    proc->cpu.running = false;
    return 0;
}
static uint32_t crt_abort(win32_process_t *proc, uint32_t *args) {
    (void)args;
    LOGE("CRT abort() called!");
    proc->cpu.running = false;
    return 0;
}

void msvcrt_init(void) {
    s_fmode_va = 0;
    s_commode_va = 0;
    s_acmdln_va = 0;
    s_initenv_va = 0;
    s_errno_va = 0;
    s_iob_va = 0;

    /* Note: msvcrt functions are __cdecl (callee does NOT unwind stack) */
    win32_register_api("msvcrt.dll", "__set_app_type", 0, crt_set_app_type, 1);
    win32_register_api("msvcrt.dll", "__p__fmode", 0, crt_p_fmode, 0);
    win32_register_api("msvcrt.dll", "__p__commode", 0, crt_p_commode, 0);
    win32_register_api("msvcrt.dll", "_controlfp", 0, crt_controlfp, 2);
    win32_register_api("msvcrt.dll", "_initterm", 0, crt_initterm, 2);
    win32_register_api("msvcrt.dll", "__getmainargs", 0, crt_getmainargs, 5);
    win32_register_api("msvcrt.dll", "_acmdln", 0, crt_acmdln, 0);
    win32_register_api("msvcrt.dll", "__p__acmdln", 0, crt_acmdln, 0);
    win32_register_api("msvcrt.dll", "__initenv", 0, crt_initenv, 0);
    win32_register_api("msvcrt.dll", "__mb_cur_max", 0, crt_mb_cur_max, 0);
    win32_register_api("msvcrt.dll", "_errno", 0, crt_errno, 0);
    win32_register_api("msvcrt.dll", "_iob", 0, crt_iob, 0);
    win32_register_api("msvcrt.dll", "_ismbblead", 0, crt_ismbblead, 1);
    win32_register_api("msvcrt.dll", "_onexit", 0, crt_onexit, 1);
    win32_register_api("msvcrt.dll", "_amsg_exit", 0, crt_amsg_exit, 1);
    win32_register_api("msvcrt.dll", "abort", 0, crt_abort, 0);
    win32_register_api("msvcrt.dll", "localeconv", 0, crt_localeconv, 0);
    win32_register_api("msvcrt.dll", "setlocale", 0, crt_setlocale, 2);
    win32_register_api("msvcrt.dll", "signal", 0, crt_signal, 2);

    win32_register_api("msvcrt.dll", "malloc", 0, crt_malloc, 1);
    win32_register_api("msvcrt.dll", "free", 0, crt_free, 1);
    win32_register_api("msvcrt.dll", "calloc", 0, crt_calloc, 2);
    win32_register_api("msvcrt.dll", "realloc", 0, crt_realloc, 2);

    win32_register_api("msvcrt.dll", "strlen", 0, crt_strlen, 1);
    win32_register_api("msvcrt.dll", "wcslen", 0, crt_wcslen, 1);
    win32_register_api("msvcrt.dll", "strcmp", 0, crt_strcmp, 2);
    win32_register_api("msvcrt.dll", "strncmp", 0, crt_strncmp, 3);
    win32_register_api("msvcrt.dll", "strcpy", 0, crt_strcpy, 2);
    win32_register_api("msvcrt.dll", "strchr", 0, crt_strchr, 2);
    win32_register_api("msvcrt.dll", "memset", 0, crt_memset, 3);
    win32_register_api("msvcrt.dll", "memcpy", 0, crt_memcpy, 3);
    win32_register_api("msvcrt.dll", "atoi", 0, crt_atoi, 1);
    win32_register_api("msvcrt.dll", "strtol", 0, crt_strtol, 3);
    win32_register_api("msvcrt.dll", "strtoul", 0, crt_strtoul, 3);
    win32_register_api("msvcrt.dll", "_strtoi64", 0, crt_strtol, 3);
    win32_register_api("msvcrt.dll", "_strtoui64", 0, crt_strtoul, 3);
    win32_register_api("msvcrt.dll", "tolower", 0, crt_tolower, 1);
    win32_register_api("msvcrt.dll", "isspace", 0, crt_isspace, 1);
    win32_register_api("msvcrt.dll", "isxdigit", 0, crt_isxdigit, 1);
    win32_register_api("msvcrt.dll", "strerror", 0, crt_strerror, 1);

    win32_register_api("msvcrt.dll", "fprintf", 0, crt_fprintf, 3);
    win32_register_api("msvcrt.dll", "fputc", 0, crt_fputc, 2);
    win32_register_api("msvcrt.dll", "fwrite", 0, crt_fwrite, 4);
    win32_register_api("msvcrt.dll", "getc", 0, crt_getc, 1);
    win32_register_api("msvcrt.dll", "ungetc", 0, crt_ungetc, 2);
    win32_register_api("msvcrt.dll", "vfprintf", 0, crt_vfprintf, 3);

    win32_register_api("msvcrt.dll", "srand", 0, crt_srand, 1);
    win32_register_api("msvcrt.dll", "rand", 0, crt_rand, 0);
    win32_register_api("msvcrt.dll", "time", 0, crt_time, 1);
    win32_register_api("msvcrt.dll", "_time32", 0, crt_time, 1);
    win32_register_api("msvcrt.dll", "exit", 0, crt_exit, 1);
    win32_register_api("msvcrt.dll", "_exit", 0, crt_exit, 1);
    win32_register_api("msvcrt.dll", "_cexit", 0, crt_dummy_zero, 0);
    win32_register_api("msvcrt.dll", "_c_exit", 0, crt_dummy_zero, 0);
    win32_register_api("msvcrt.dll", "_adjust_fdiv", 0, crt_dummy_zero, 0);
    win32_register_api("msvcrt.dll", "__setusermatherr", 0, crt_dummy_zero, 1);
    win32_register_api("msvcrt.dll", "_XcptFilter", 0, crt_dummy_zero, 2);
    win32_register_api("msvcrt.dll", "_except_handler3", 0, crt_dummy_zero, 0);
}
