#include "msvcrt_thunks.h"
#include "hle.h"
#include "memory.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static uint32_t s_iob_addr = 0;
static uint32_t s_argc = 0;
static uint32_t s_argv_addr = 0;
static uint32_t s_envp_addr = 0;
static uint32_t s_fmode = 0;
static uint32_t s_commode = 0;
static uint32_t s_environ_ptr = 0;
static uint32_t s_errno_val = 0;

uint32_t msvcrt_get_iob_addr(void) {
    if (!s_iob_addr) {
        s_iob_addr = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        // Put distinct markers for stdin (0), stdout (1), stderr (2)
        mem_write_u32(s_iob_addr, 0x0);       // stdin
        mem_write_u32(s_iob_addr + 32, 0x1);  // stdout
        mem_write_u32(s_iob_addr + 64, 0x2);  // stderr
    }
    return s_iob_addr;
}

void msvcrt_init_cmdline(int argc, char** argv) {
    msvcrt_get_iob_addr();

    s_argc = argc > 0 ? (uint32_t)argc : 1;
    // Allocate space for argv pointers and string contents
    uint32_t total_str_bytes = 0;
    for (int i = 0; i < argc; i++) {
        total_str_bytes += strlen(argv[i]) + 1;
    }
    if (argc == 0) total_str_bytes += 16;

    uint32_t alloc_size = (s_argc + 2) * 4 + total_str_bytes + 256;
    uint32_t base = mem_virtual_alloc(0, (alloc_size + 0xFFF) & ~0xFFF, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_argv_addr = base;

    uint32_t str_cursor = base + (s_argc + 1) * 4;
    for (int i = 0; i < argc; i++) {
        mem_write_u32(s_argv_addr + (i * 4), str_cursor);
        size_t len = strlen(argv[i]) + 1;
        mem_write(str_cursor, argv[i], len);
        str_cursor += len;
    }
    if (argc == 0) {
        mem_write_u32(s_argv_addr, str_cursor);
        mem_write(str_cursor, "app.exe", 8);
        str_cursor += 8;
    }
    mem_write_u32(s_argv_addr + (s_argc * 4), 0); // NULL terminator

    // Empty envp
    s_envp_addr = str_cursor;
    mem_write_u32(s_envp_addr, 0);
}

static uint32_t hle_p_iob(struct CPUState* cpu, uint32_t* args) {
    return msvcrt_get_iob_addr();
}

static uint32_t hle_getmainargs(struct CPUState* cpu, uint32_t* args) {
    uint32_t p_argc = args[0];
    uint32_t p_argv = args[1];
    uint32_t p_envp = args[2];
    // uint32_t dowildcard = args[3];
    // uint32_t p_startup = args[4];

    if (p_argc) mem_write_u32(p_argc, s_argc);
    if (p_argv) mem_write_u32(p_argv, s_argv_addr);
    if (p_envp) mem_write_u32(p_envp, s_envp_addr);
    return 0;
}

static uint32_t hle_p_argv(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_ptr_box = 0;
    if (!s_ptr_box) {
        s_ptr_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_ptr_box, s_argv_addr);
    return s_ptr_box;
}

static uint32_t hle_p_argc(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_argc_box = 0;
    if (!s_argc_box) {
        s_argc_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_argc_box, s_argc);
    return s_argc_box;
}

static uint32_t hle_p_initenv(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_env_box = 0;
    if (!s_env_box) {
        s_env_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_env_box, s_envp_addr);
    return s_env_box;
}

static uint32_t hle_p_fmode(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_fmode_box = 0;
    if (!s_fmode_box) {
        s_fmode_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_fmode_box, s_fmode);
    return s_fmode_box;
}

static uint32_t hle_p_commode(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_commode_box = 0;
    if (!s_commode_box) {
        s_commode_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_commode_box, s_commode);
    return s_commode_box;
}

static uint32_t hle_p_environ(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_environ_box = 0;
    if (!s_environ_box) {
        s_environ_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    mem_write_u32(s_environ_box, s_environ_ptr);
    return s_environ_box;
}

static uint32_t hle_errno_func(struct CPUState* cpu, uint32_t* args) {
    static uint32_t s_errno_box = 0;
    if (!s_errno_box) {
        s_errno_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    return s_errno_box;
}

static uint32_t hle_set_app_type(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_setusermatherr(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_initterm(struct CPUState* cpu, uint32_t* args) {
    DWORD start = args[0];
    DWORD end = args[1];
    if (!start || !end) return 0;
    for (DWORD p = start; p < end; p += 4) {
        DWORD fn = mem_read_u32(p);
        if (fn != 0) {
            // Function pointer to static initializer
        }
    }
    return 0;
}

static uint32_t hle_exit(struct CPUState* cpu, uint32_t* args) {
    extern void cpu_halt(struct CPUState* cpu, uint32_t exit_code);
    cpu_halt(cpu, args[0]);
    return 0;
}

static uint32_t hle_abort(struct CPUState* cpu, uint32_t* args) {
    extern void cpu_halt(struct CPUState* cpu, uint32_t exit_code);
    cpu_halt(cpu, 3);
    return 0;
}

static uint32_t hle_amsg_exit(struct CPUState* cpu, uint32_t* args) {
    extern void cpu_halt(struct CPUState* cpu, uint32_t exit_code);
    cpu_halt(cpu, args[0]);
    return 0;
}

static uint32_t hle_puts(struct CPUState* cpu, uint32_t* args) {
    const char* str = (const char*)mem_to_host(args[0]);
    if (str) {
        puts(str);
        fflush(stdout);
        return 1;
    }
    return 0;
}

static uint32_t hle_putchar(struct CPUState* cpu, uint32_t* args) {
    putchar((int)(args[0] & 0xFF));
    fflush(stdout);
    return args[0] & 0xFF;
}

static void format_and_print(FILE* fp, const char* fmt, struct CPUState* cpu, uint32_t arg_offset) {
    if (!fmt) return;
    const char* p = fmt;
    uint32_t arg_idx = 0;

    while (*p) {
        if (*p == '%' && *(p + 1) != '\0') {
            p++;
            if (*p == '%') {
                fputc('%', fp);
                p++;
                continue;
            }
            // Parse flags/width/length specifiers
            while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' ||
                   isdigit((unsigned char)*p) || *p == '.' || *p == 'h' || *p == 'l' || *p == 'z') {
                p++;
            }
            char spec = *p++;
            uint32_t val = mem_read_u32(cpu->esp + arg_offset + (arg_idx * 4));
            arg_idx++;

            switch (spec) {
                case 's': {
                    const char* s = (const char*)mem_to_host(val);
                    if (s) fputs(s, fp);
                    else fputs("(null)", fp);
                    break;
                }
                case 'd':
                case 'i':
                    fprintf(fp, "%d", (int32_t)val);
                    break;
                case 'u':
                    fprintf(fp, "%u", (uint32_t)val);
                    break;
                case 'x':
                    fprintf(fp, "%x", (uint32_t)val);
                    break;
                case 'X':
                    fprintf(fp, "%X", (uint32_t)val);
                    break;
                case 'p':
                    fprintf(fp, "%08X", (uint32_t)val);
                    break;
                case 'c':
                    fputc((int)(val & 0xFF), fp);
                    break;
                default:
                    fputc(spec, fp);
                    break;
            }
        } else {
            fputc(*p++, fp);
        }
    }
    fflush(fp);
}

static uint32_t hle_printf(struct CPUState* cpu, uint32_t* args) {
    const char* fmt = (const char*)mem_to_host(args[0]);
    // Format string is args[0] at esp+4. Arguments follow at esp+8 (offset 8)
    format_and_print(stdout, fmt, cpu, 8);
    return 0;
}

static uint32_t hle_fprintf(struct CPUState* cpu, uint32_t* args) {
    // uint32_t fp_val = args[0];
    const char* fmt = (const char*)mem_to_host(args[1]);
    FILE* fp = stdout;
    if (args[0] == s_iob_addr + 64) fp = stderr;
    // fp at esp+4, fmt at esp+8. Arguments follow at esp+12 (offset 12)
    format_and_print(fp, fmt, cpu, 12);
    return 0;
}

static uint32_t hle_vfprintf(struct CPUState* cpu, uint32_t* args) {
    const char* fmt = (const char*)mem_to_host(args[1]);
    FILE* fp = stdout;
    if (args[0] == s_iob_addr + 64) fp = stderr;
    uint32_t va_list_addr = args[2];
    if (va_list_addr && fmt) {
        // va_list points to the arguments array in Win32 memory
        format_and_print(fp, fmt, cpu, va_list_addr - cpu->esp);
    }
    return 0;
}

static uint32_t hle_fputs(struct CPUState* cpu, uint32_t* args) {
    const char* str = (const char*)mem_to_host(args[0]);
    FILE* fp = stdout;
    if (args[1] == s_iob_addr + 64) fp = stderr;
    if (str) {
        fputs(str, fp);
        fflush(fp);
        return 1;
    }
    return 0;
}

static uint32_t hle_fputc(struct CPUState* cpu, uint32_t* args) {
    FILE* fp = stdout;
    if (args[1] == s_iob_addr + 64) fp = stderr;
    fputc((int)(args[0] & 0xFF), fp);
    return args[0] & 0xFF;
}

static uint32_t hle_fwrite(struct CPUState* cpu, uint32_t* args) {
    const void* ptr = mem_to_host(args[0]);
    size_t size = args[1];
    size_t count = args[2];
    FILE* fp = stdout;
    if (args[3] == s_iob_addr + 64) fp = stderr;
    if (ptr && size && count) {
        size_t written = fwrite(ptr, size, count, fp);
        fflush(fp);
        return (uint32_t)written;
    }
    return count;
}

static uint32_t hle_fflush(struct CPUState* cpu, uint32_t* args) {
    FILE* fp = stdout;
    if (args[0] == s_iob_addr + 64) fp = stderr;
    fflush(fp);
    return 0;
}

static uint32_t hle_malloc(struct CPUState* cpu, uint32_t* args) {
    SIZE_T size = args[0];
    if (size == 0) size = 1;
    return mem_virtual_alloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static uint32_t hle_calloc(struct CPUState* cpu, uint32_t* args) {
    SIZE_T num = args[0];
    SIZE_T size = args[1];
    SIZE_T total = num * size;
    if (total == 0) total = 1;
    return mem_virtual_alloc(0, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static uint32_t hle_realloc(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    SIZE_T size = args[1];
    if (!ptr) return mem_virtual_alloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (size == 0) {
        mem_virtual_free(ptr, 0, MEM_RELEASE);
        return 0;
    }
    uint32_t new_ptr = mem_virtual_alloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* host_src = mem_to_host(ptr);
    void* host_dst = mem_to_host(new_ptr);
    if (host_src && host_dst) {
        memcpy(host_dst, host_src, size);
    }
    mem_virtual_free(ptr, 0, MEM_RELEASE);
    return new_ptr;
}

static uint32_t hle_free(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    if (ptr) mem_virtual_free(ptr, 0, MEM_RELEASE);
    return 0;
}

static uint32_t hle_strlen(struct CPUState* cpu, uint32_t* args) {
    if (!args[0]) return 0;
    const char* s = (const char*)mem_to_host(args[0]);
    return s ? (uint32_t)strlen(s) : 0;
}

static uint32_t hle_strcmp(struct CPUState* cpu, uint32_t* args) {
    const char* s1 = (const char*)mem_to_host(args[0]);
    const char* s2 = (const char*)mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)strcmp(s1, s2);
}

static uint32_t hle_strncmp(struct CPUState* cpu, uint32_t* args) {
    const char* s1 = (const char*)mem_to_host(args[0]);
    const char* s2 = (const char*)mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)strncmp(s1, s2, args[2]);
}

static uint32_t hle_stricmp(struct CPUState* cpu, uint32_t* args) {
    const char* s1 = (const char*)mem_to_host(args[0]);
    const char* s2 = (const char*)mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)strcasecmp(s1, s2);
}

static uint32_t hle_strnicmp(struct CPUState* cpu, uint32_t* args) {
    const char* s1 = (const char*)mem_to_host(args[0]);
    const char* s2 = (const char*)mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)strncasecmp(s1, s2, args[2]);
}

static uint32_t hle_strcpy(struct CPUState* cpu, uint32_t* args) {
    uint32_t dst_addr = args[0];
    uint32_t src_addr = args[1];
    if (!dst_addr || !src_addr) return dst_addr;
    char* dst = (char*)mem_to_host(dst_addr);
    const char* src = (const char*)mem_to_host(src_addr);
    if (dst && src) {
        size_t max_len = 65536;
        size_t i = 0;
        while (src[i] != '\0' && i < max_len) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }
    return dst_addr;
}

static uint32_t hle_strncpy(struct CPUState* cpu, uint32_t* args) {
    uint32_t dst_addr = args[0];
    uint32_t src_addr = args[1];
    uint32_t count = args[2];
    if (!dst_addr || !src_addr || count == 0) return dst_addr;

    char* dst = (char*)mem_to_host(dst_addr);
    const char* src = (const char*)mem_to_host(src_addr);
    if (!dst || !src) return dst_addr;

    for (uint32_t i = 0; i < count; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            for (uint32_t j = i + 1; j < count; j++) dst[j] = '\0';
            break;
        }
    }
    return dst_addr;
}

static uint32_t hle_strchr(struct CPUState* cpu, uint32_t* args) {
    const char* s = (const char*)mem_to_host(args[0]);
    if (!s) return 0;
    char* found = strchr(s, (int)args[1]);
    if (!found) return 0;
    return args[0] + (uint32_t)(found - s);
}

static uint32_t hle_strrchr(struct CPUState* cpu, uint32_t* args) {
    const char* s = (const char*)mem_to_host(args[0]);
    if (!s) return 0;
    char* found = strrchr(s, (int)args[1]);
    if (!found) return 0;
    return args[0] + (uint32_t)(found - s);
}

static uint32_t hle_strstr(struct CPUState* cpu, uint32_t* args) {
    const char* s1 = (const char*)mem_to_host(args[0]);
    const char* s2 = (const char*)mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    char* found = strstr(s1, s2);
    if (!found) return 0;
    return args[0] + (uint32_t)(found - s1);
}

static uint32_t hle_memcpy(struct CPUState* cpu, uint32_t* args) {
    void* dst = mem_to_host(args[0]);
    const void* src = mem_to_host(args[1]);
    if (dst && src && args[2]) memcpy(dst, src, args[2]);
    return args[0];
}

static uint32_t hle_memmove(struct CPUState* cpu, uint32_t* args) {
    void* dst = mem_to_host(args[0]);
    const void* src = mem_to_host(args[1]);
    if (dst && src && args[2]) memmove(dst, src, args[2]);
    return args[0];
}

static uint32_t hle_memset(struct CPUState* cpu, uint32_t* args) {
    void* dst = mem_to_host(args[0]);
    if (dst && args[2]) memset(dst, (int)args[1], args[2]);
    return args[0];
}

static uint32_t hle_memcmp(struct CPUState* cpu, uint32_t* args) {
    const void* s1 = mem_to_host(args[0]);
    const void* s2 = mem_to_host(args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)memcmp(s1, s2, args[2]);
}

static uint32_t hle_isctype(struct CPUState* cpu, uint32_t* args) {
    int c = (int)args[0];
    uint32_t mask = args[1];
    uint32_t ret = 0;
    if (isalpha(c)) ret |= 0x0103; // _ALPHA | _UPPER | _LOWER
    if (isdigit(c)) ret |= 0x0004; // _DIGIT
    if (isspace(c)) ret |= 0x0008; // _SPACE
    if (ispunct(c)) ret |= 0x0010; // _PUNCT
    if (iscntrl(c)) ret |= 0x0020; // _CONTROL
    if (isxdigit(c)) ret |= 0x0080; // _HEX
    return (ret & mask) ? 1 : 0;
}

static uint32_t hle_isalnum(struct CPUState* cpu, uint32_t* args) { return isalnum((int)args[0]) ? 1 : 0; }
static uint32_t hle_isalpha(struct CPUState* cpu, uint32_t* args) { return isalpha((int)args[0]) ? 1 : 0; }
static uint32_t hle_isdigit(struct CPUState* cpu, uint32_t* args) { return isdigit((int)args[0]) ? 1 : 0; }
static uint32_t hle_isspace(struct CPUState* cpu, uint32_t* args) { return isspace((int)args[0]) ? 1 : 0; }
static uint32_t hle_islower(struct CPUState* cpu, uint32_t* args) { return islower((int)args[0]) ? 1 : 0; }
static uint32_t hle_isupper(struct CPUState* cpu, uint32_t* args) { return isupper((int)args[0]) ? 1 : 0; }
static uint32_t hle_isxdigit(struct CPUState* cpu, uint32_t* args) { return isxdigit((int)args[0]) ? 1 : 0; }
static uint32_t hle_tolower(struct CPUState* cpu, uint32_t* args) { return (uint32_t)tolower((int)args[0]); }
static uint32_t hle_toupper(struct CPUState* cpu, uint32_t* args) { return (uint32_t)toupper((int)args[0]); }

static uint32_t hle_getenv(struct CPUState* cpu, uint32_t* args) {
    const char* name = (const char*)mem_to_host(args[0]);
    if (!name) return 0;
    const char* val = getenv(name);
    if (!val) return 0;
    uint32_t len = (uint32_t)strlen(val) + 1;
    uint32_t addr = mem_virtual_alloc(0, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    mem_write(addr, val, len);
    return addr;
}

static uint32_t hle_time(struct CPUState* cpu, uint32_t* args) {
    time_t t = time(NULL);
    if (args[0]) mem_write_u32(args[0], (uint32_t)t);
    return (uint32_t)t;
}

static uint32_t hle_signal(struct CPUState* cpu, uint32_t* args) { return 0; }
static uint32_t hle_atexit(struct CPUState* cpu, uint32_t* args) { return 0; }
static uint32_t hle_lock(struct CPUState* cpu, uint32_t* args) { return 0; }
static uint32_t hle_unlock(struct CPUState* cpu, uint32_t* args) { return 0; }

static uint32_t hle_isatty(struct CPUState* cpu, uint32_t* args) { return 1; }
static uint32_t hle_fileno(struct CPUState* cpu, uint32_t* args) {
    if (args[0] == s_iob_addr) return 0;
    if (args[0] == s_iob_addr + 32) return 1;
    if (args[0] == s_iob_addr + 64) return 2;
    return 1;
}
static uint32_t hle_setmode(struct CPUState* cpu, uint32_t* args) { return 0x8000; }

static uint32_t hle_strerror(struct CPUState* cpu, uint32_t* args) {
    int err = (int)args[0];
    const char* msg = strerror(err);
    if (!msg) msg = "Unknown error";
    static DWORD s_strerror_buf = 0;
    if (!s_strerror_buf) {
        s_strerror_buf = mem_virtual_alloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    char* dst = (char*)mem_to_host(s_strerror_buf);
    if (dst) {
        strncpy(dst, msg, 4095);
        dst[4095] = '\0';
    }
    return s_strerror_buf;
}

static uint32_t hle_strcat(struct CPUState* cpu, uint32_t* args) {
    char* dst = (char*)mem_to_host(args[0]);
    const char* src = (const char*)mem_to_host(args[1]);
    if (dst && src) strcat(dst, src);
    return args[0];
}

static uint32_t hle_strpbrk(struct CPUState* cpu, uint32_t* args) {
    const char* s = (const char*)mem_to_host(args[0]);
    const char* accept = (const char*)mem_to_host(args[1]);
    if (!s || !accept) return 0;
    const char* res = strpbrk(s, accept);
    return res ? (uint32_t)(args[0] + (res - s)) : 0;
}

static uint32_t hle_strtok(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_last_strtok = 0;
    const char* delim = (const char*)mem_to_host(args[1]);
    if (!delim) return 0;
    if (args[0] != 0) {
        s_last_strtok = args[0];
    } else {
        if (!s_last_strtok) return 0;
    }
    char* s = (char*)mem_to_host(s_last_strtok);
    if (!s) return 0;
    while (*s && strchr(delim, *s)) {
        s++;
        s_last_strtok++;
    }
    if (!*s) {
        s_last_strtok = 0;
        return 0;
    }
    DWORD token_start = s_last_strtok;
    while (*s && !strchr(delim, *s)) {
        s++;
        s_last_strtok++;
    }
    if (*s) {
        *s = '\0';
        s_last_strtok++;
    }
    return token_start;
}

static uint32_t hle_strtoul(struct CPUState* cpu, uint32_t* args) {
    const char* nptr = (const char*)mem_to_host(args[0]);
    if (!nptr) return 0;
    char* endptr = NULL;
    int base = (int)args[2];
    unsigned long res = strtoul(nptr, &endptr, base);
    if (args[1]) {
        uint32_t* out_end = (uint32_t*)mem_to_host(args[1]);
        if (out_end) {
            *out_end = args[0] + (uint32_t)(endptr - nptr);
        }
    }
    return (uint32_t)res;
}

static uint32_t hle_strdup(struct CPUState* cpu, uint32_t* args) {
    const char* s = (const char*)mem_to_host(args[0]);
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    DWORD dst = mem_virtual_alloc(0, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (dst) {
        char* d = (char*)mem_to_host(dst);
        memcpy(d, s, len);
    }
    return dst;
}

static uint32_t hle_setlocale(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_locale_str = 0;
    if (!s_locale_str) {
        s_locale_str = mem_virtual_alloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        char* p = (char*)mem_to_host(s_locale_str);
        if (p) strcpy(p, "C");
    }
    return s_locale_str;
}

static uint32_t hle_localeconv(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_lconv = 0;
    if (!s_lconv) {
        s_lconv = mem_virtual_alloc(0, 128, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        char* p = (char*)mem_to_host(s_lconv);
        if (p) {
            strcpy(p + 64, ".");
            *(uint32_t*)p = s_lconv + 64;
        }
    }
    return s_lconv;
}

static uint32_t hle_get_osfhandle(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    if (fd == 0) return 100;
    if (fd == 1) return 101;
    if (fd == 2) return 102;
    return (uint32_t)fd;
}

static uint32_t hle_open_osfhandle(struct CPUState* cpu, uint32_t* args) {
    return args[0];
}

static uint32_t hle_p_daylight(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_daylight = 0;
    if (!s_daylight) {
        s_daylight = mem_virtual_alloc(0, 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    return s_daylight;
}

static uint32_t hle_p_dstbias(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_dstbias = 0;
    if (!s_dstbias) {
        s_dstbias = mem_virtual_alloc(0, 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        int* p = (int*)mem_to_host(s_dstbias);
        if (p) *p = -3600;
    }
    return s_dstbias;
}

static uint32_t hle_p_timezone(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_tz = 0;
    if (!s_tz) {
        s_tz = mem_virtual_alloc(0, 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    return s_tz;
}

static uint32_t hle_p_mb_cur_max(struct CPUState* cpu, uint32_t* args) {
    static DWORD s_mb_cur_max = 0;
    if (!s_mb_cur_max) {
        s_mb_cur_max = mem_virtual_alloc(0, 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        int* p = (int*)mem_to_host(s_mb_cur_max);
        if (p) *p = 1;
    }
    return s_mb_cur_max;
}

static uint32_t hle_tzset(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_stat(struct CPUState* cpu, uint32_t* args) {
    const char* path = (const char*)mem_to_host(args[0]);
    void* stat_buf = mem_to_host(args[1]);
    if (!path || !stat_buf) return -1;
    struct stat st;
    if (stat(path, &st) == 0) {
        uint8_t* p = (uint8_t*)stat_buf;
        memset(p, 0, 36);
        *(uint32_t*)(p + 0) = st.st_dev;
        *(uint16_t*)(p + 4) = st.st_ino;
        *(uint16_t*)(p + 6) = (uint16_t)st.st_mode;
        *(int16_t*)(p + 8) = st.st_nlink;
        *(int16_t*)(p + 10) = st.st_uid;
        *(int16_t*)(p + 12) = st.st_gid;
        *(uint32_t*)(p + 14) = st.st_rdev;
        *(uint32_t*)(p + 18) = (uint32_t)st.st_size;
        *(uint32_t*)(p + 22) = (uint32_t)st.st_atime;
        *(uint32_t*)(p + 26) = (uint32_t)st.st_mtime;
        *(uint32_t*)(p + 30) = (uint32_t)st.st_ctime;
        return 0;
    }
    return -1;
}

static uint32_t hle_fstat(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    void* stat_buf = mem_to_host(args[1]);
    if (!stat_buf) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        uint8_t* p = (uint8_t*)stat_buf;
        memset(p, 0, 36);
        *(uint32_t*)(p + 0) = st.st_dev;
        *(uint16_t*)(p + 4) = st.st_ino;
        *(uint16_t*)(p + 6) = (uint16_t)st.st_mode;
        *(int16_t*)(p + 8) = st.st_nlink;
        *(int16_t*)(p + 10) = st.st_uid;
        *(int16_t*)(p + 12) = st.st_gid;
        *(uint32_t*)(p + 14) = st.st_rdev;
        *(uint32_t*)(p + 18) = (uint32_t)st.st_size;
        *(uint32_t*)(p + 22) = (uint32_t)st.st_atime;
        *(uint32_t*)(p + 26) = (uint32_t)st.st_mtime;
        *(uint32_t*)(p + 30) = (uint32_t)st.st_ctime;
        return 0;
    }
    return -1;
}

static uint32_t hle_access(struct CPUState* cpu, uint32_t* args) {
    const char* path = (const char*)mem_to_host(args[0]);
    int mode = (int)args[1];
    if (!path) return -1;
    return access(path, mode);
}

static uint32_t hle_getcwd(struct CPUState* cpu, uint32_t* args) {
    char* buf = (char*)mem_to_host(args[0]);
    int maxlen = (int)args[1];
    if (!buf || maxlen <= 0) return 0;
    if (getcwd(buf, maxlen)) return args[0];
    return 0;
}

static uint32_t hle_write(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    const void* buf = mem_to_host(args[1]);
    size_t count = args[2];
    if (!buf) return 0;
    return (uint32_t)write(fd, buf, count);
}

static uint32_t hle_read(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    void* buf = mem_to_host(args[1]);
    size_t count = args[2];
    if (!buf) return 0;
    return (uint32_t)read(fd, buf, count);
}

static uint32_t hle_lseeki64(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    int64_t offset = (int64_t)args[1] | ((int64_t)args[2] << 32);
    int origin = (int)args[3];
    off_t res = lseek(fd, offset, origin);
    cpu->edx = (uint32_t)((uint64_t)res >> 32);
    return (uint32_t)res;
}

static uint32_t hle_telli64(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    off_t res = lseek(fd, 0, SEEK_CUR);
    cpu->edx = (uint32_t)((uint64_t)res >> 32);
    return (uint32_t)res;
}

static uint32_t hle_wcslen(struct CPUState* cpu, uint32_t* args) {
    if (!args[0]) return 0;
    const uint16_t* s = (const uint16_t*)mem_to_host(args[0]);
    if (!s) return 0;
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

static uint32_t hle_fullpath(struct CPUState* cpu, uint32_t* args) {
    const char* rel = (const char*)mem_to_host(args[1]);
    char* abs = (char*)mem_to_host(args[0]);
    if (!rel) return 0;
    if (!abs) {
        DWORD buf = mem_virtual_alloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        abs = (char*)mem_to_host(buf);
#ifdef _WIN32
        if (_fullpath(abs, rel, 4095) == NULL) {
            strncpy(abs, rel, 4095);
        }
#else
        if (realpath(rel, abs) == NULL) {
            strncpy(abs, rel, 4095);
        }
#endif
        return buf;
    }
#ifdef _WIN32
    if (_fullpath(abs, rel, args[2] ? args[2] - 1 : 4095) == NULL) {
        strncpy(abs, rel, args[2] ? args[2] - 1 : 4095);
    }
#else
    if (realpath(rel, abs) == NULL) {
        strncpy(abs, rel, args[2] ? args[2] - 1 : 4095);
    }
#endif
    return args[0];
}

static uint32_t hle_chdir(struct CPUState* cpu, uint32_t* args) {
    const char* path = (const char*)mem_to_host(args[0]);
    if (!path) return -1;
    return chdir(path);
}

static uint32_t hle_close(struct CPUState* cpu, uint32_t* args) {
    int fd = (int)args[0];
    if (fd <= 2) return 0;
    return close(fd);
}

static uint32_t hle_unlink(struct CPUState* cpu, uint32_t* args) {
    const char* path = (const char*)mem_to_host(args[0]);
    if (!path) return -1;
    return unlink(path);
}

static uint32_t hle_umask(struct CPUState* cpu, uint32_t* args) {
    return umask((mode_t)args[0]);
}

void register_msvcrt_apis(void) {
    msvcrt_get_iob_addr();

    hle_register("msvcrt.dll", "__p__iob", 0, hle_p_iob, 0, true);
    hle_register("msvcrt.dll", "__getmainargs", 0, hle_getmainargs, 5, true);
    hle_register("msvcrt.dll", "__p___argv", 0, hle_p_argv, 0, true);
    hle_register("msvcrt.dll", "__p___argc", 0, hle_p_argc, 0, true);
    hle_register("msvcrt.dll", "__p___initenv", 0, hle_p_initenv, 0, true);
    hle_register("msvcrt.dll", "__p__fmode", 0, hle_p_fmode, 0, true);
    hle_register("msvcrt.dll", "__p__commode", 0, hle_p_commode, 0, true);
    hle_register("msvcrt.dll", "__p__environ", 0, hle_p_environ, 0, true);
    hle_register("msvcrt.dll", "_errno", 0, hle_errno_func, 0, true);

    hle_register("msvcrt.dll", "__set_app_type", 0, hle_set_app_type, 1, true);
    hle_register("msvcrt.dll", "__setusermatherr", 0, hle_setusermatherr, 1, true);
    hle_register("msvcrt.dll", "_initterm", 0, hle_initterm, 2, true);
    hle_register("msvcrt.dll", "_initterm_e", 0, hle_initterm, 2, true);

    hle_register("msvcrt.dll", "exit", 0, hle_exit, 1, true);
    hle_register("msvcrt.dll", "_exit", 0, hle_exit, 1, true);
    hle_register("msvcrt.dll", "_cexit", 0, hle_exit, 0, true);
    hle_register("msvcrt.dll", "_c_exit", 0, hle_exit, 0, true);
    hle_register("msvcrt.dll", "_amsg_exit", 0, hle_amsg_exit, 1, true);
    hle_register("msvcrt.dll", "abort", 0, hle_abort, 0, true);

    hle_register("msvcrt.dll", "puts", 0, hle_puts, 1, true);
    hle_register("msvcrt.dll", "putchar", 0, hle_putchar, 1, true);
    hle_register("msvcrt.dll", "printf", 0, hle_printf, 1, true);
    hle_register("msvcrt.dll", "fprintf", 0, hle_fprintf, 2, true);
    hle_register("msvcrt.dll", "vfprintf", 0, hle_vfprintf, 3, true);
    hle_register("msvcrt.dll", "fputs", 0, hle_fputs, 2, true);
    hle_register("msvcrt.dll", "fputc", 0, hle_fputc, 2, true);
    hle_register("msvcrt.dll", "fwrite", 0, hle_fwrite, 4, true);
    hle_register("msvcrt.dll", "fflush", 0, hle_fflush, 1, true);

    hle_register("msvcrt.dll", "malloc", 0, hle_malloc, 1, true);
    hle_register("msvcrt.dll", "calloc", 0, hle_calloc, 2, true);
    hle_register("msvcrt.dll", "realloc", 0, hle_realloc, 2, true);
    hle_register("msvcrt.dll", "free", 0, hle_free, 1, true);

    hle_register("msvcrt.dll", "strlen", 0, hle_strlen, 1, true);
    hle_register("msvcrt.dll", "strcmp", 0, hle_strcmp, 2, true);
    hle_register("msvcrt.dll", "strncmp", 0, hle_strncmp, 3, true);
    hle_register("msvcrt.dll", "_stricmp", 0, hle_stricmp, 2, true);
    hle_register("msvcrt.dll", "_strnicmp", 0, hle_strnicmp, 3, true);
    hle_register("msvcrt.dll", "strcpy", 0, hle_strcpy, 2, true);
    hle_register("msvcrt.dll", "strncpy", 0, hle_strncpy, 3, true);
    hle_register("msvcrt.dll", "strchr", 0, hle_strchr, 2, true);
    hle_register("msvcrt.dll", "strrchr", 0, hle_strrchr, 2, true);
    hle_register("msvcrt.dll", "strstr", 0, hle_strstr, 2, true);
    hle_register("msvcrt.dll", "strcat", 0, hle_strcat, 2, true);
    hle_register("msvcrt.dll", "strerror", 0, hle_strerror, 1, true);
    hle_register("msvcrt.dll", "strpbrk", 0, hle_strpbrk, 2, true);
    hle_register("msvcrt.dll", "strtok", 0, hle_strtok, 2, true);
    hle_register("msvcrt.dll", "strtoul", 0, hle_strtoul, 3, true);
    hle_register("msvcrt.dll", "_strdup", 0, hle_strdup, 1, true);
    hle_register("msvcrt.dll", "setlocale", 0, hle_setlocale, 2, true);
    hle_register("msvcrt.dll", "localeconv", 0, hle_localeconv, 0, true);

    hle_register("msvcrt.dll", "memcpy", 0, hle_memcpy, 3, true);
    hle_register("msvcrt.dll", "memmove", 0, hle_memmove, 3, true);
    hle_register("msvcrt.dll", "memset", 0, hle_memset, 3, true);
    hle_register("msvcrt.dll", "memcmp", 0, hle_memcmp, 3, true);

    hle_register("msvcrt.dll", "_isctype", 0, hle_isctype, 2, true);
    hle_register("msvcrt.dll", "isalnum", 0, hle_isalnum, 1, true);
    hle_register("msvcrt.dll", "isalpha", 0, hle_isalpha, 1, true);
    hle_register("msvcrt.dll", "isdigit", 0, hle_isdigit, 1, true);
    hle_register("msvcrt.dll", "isspace", 0, hle_isspace, 1, true);
    hle_register("msvcrt.dll", "islower", 0, hle_islower, 1, true);
    hle_register("msvcrt.dll", "isupper", 0, hle_isupper, 1, true);
    hle_register("msvcrt.dll", "isxdigit", 0, hle_isxdigit, 1, true);
    hle_register("msvcrt.dll", "tolower", 0, hle_tolower, 1, true);
    hle_register("msvcrt.dll", "toupper", 0, hle_toupper, 1, true);

    hle_register("msvcrt.dll", "getenv", 0, hle_getenv, 1, true);
    hle_register("msvcrt.dll", "time", 0, hle_time, 1, true);
    hle_register("msvcrt.dll", "signal", 0, hle_signal, 2, true);
    hle_register("msvcrt.dll", "atexit", 0, hle_atexit, 1, true);
    hle_register("msvcrt.dll", "_lock", 0, hle_lock, 1, true);
    hle_register("msvcrt.dll", "_unlock", 0, hle_unlock, 1, true);

    hle_register("msvcrt.dll", "_isatty", 0, hle_isatty, 1, true);
    hle_register("msvcrt.dll", "_fileno", 0, hle_fileno, 1, true);
    hle_register("msvcrt.dll", "_setmode", 0, hle_setmode, 2, true);
    hle_register("msvcrt.dll", "_get_osfhandle", 0, hle_get_osfhandle, 1, true);
    hle_register("msvcrt.dll", "_open_osfhandle", 0, hle_open_osfhandle, 2, true);

    hle_register("msvcrt.dll", "__p__daylight", 0, hle_p_daylight, 0, true);
    hle_register("msvcrt.dll", "__p__dstbias", 0, hle_p_dstbias, 0, true);
    hle_register("msvcrt.dll", "__p__timezone", 0, hle_p_timezone, 0, true);
    hle_register("msvcrt.dll", "__p___mb_cur_max", 0, hle_p_mb_cur_max, 0, true);
    hle_register("msvcrt.dll", "_tzset", 0, hle_tzset, 0, true);

    hle_register("msvcrt.dll", "_stat", 0, hle_stat, 2, true);
    hle_register("msvcrt.dll", "_fstat", 0, hle_fstat, 2, true);
    hle_register("msvcrt.dll", "_access", 0, hle_access, 2, true);
    hle_register("msvcrt.dll", "_getcwd", 0, hle_getcwd, 2, true);
    hle_register("msvcrt.dll", "_write", 0, hle_write, 3, true);
    hle_register("msvcrt.dll", "_read", 0, hle_read, 3, true);
    hle_register("msvcrt.dll", "_lseeki64", 0, hle_lseeki64, 4, true);
    hle_register("msvcrt.dll", "_telli64", 0, hle_telli64, 1, true);
    hle_register("msvcrt.dll", "wcslen", 0, hle_wcslen, 1, true);
    hle_register("msvcrt.dll", "_fullpath", 0, hle_fullpath, 3, true);
    hle_register("msvcrt.dll", "_chdir", 0, hle_chdir, 1, true);
    hle_register("msvcrt.dll", "_close", 0, hle_close, 1, true);
    hle_register("msvcrt.dll", "_unlink", 0, hle_unlink, 1, true);
    hle_register("msvcrt.dll", "_umask", 0, hle_umask, 1, true);
}
