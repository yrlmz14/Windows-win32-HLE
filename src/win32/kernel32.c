#include "kernel32.h"
#include "win32_dispatch.h"
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

/* Handle table for file descriptors */
#define MAX_HANDLES 128
static FILE *s_handles[MAX_HANDLES];

static H_HANDLE alloc_file_handle(FILE *f) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!s_handles[i]) {
            s_handles[i] = f;
            return (H_HANDLE)(0x1000 + i);
        }
    }
    return 0;
}

static FILE *get_file_handle(H_HANDLE h) {
    uint32_t idx = (uint32_t)h - 0x1000;
    if (idx > 0 && idx < MAX_HANDLES) return s_handles[idx];
    return NULL;
}

static void free_file_handle(H_HANDLE h) {
    uint32_t idx = (uint32_t)h - 0x1000;
    if (idx > 0 && idx < MAX_HANDLES && s_handles[idx]) {
        fclose(s_handles[idx]);
        s_handles[idx] = NULL;
    }
}

/* Path translation: maps C:\path to local storage */
void win32_translate_path(const char *win_path, char *out_host, size_t out_sz) {
    if (!win_path || !out_host || out_sz == 0) return;
    if (strncmp(win_path, "C:\\", 3) == 0 || strncmp(win_path, "c:\\", 3) == 0) {
        snprintf(out_host, out_sz, "apps/%s", win_path + 3);
    } else if (strncmp(win_path, "apps/", 5) == 0 || strncmp(win_path, "apps\\", 5) == 0) {
        snprintf(out_host, out_sz, "%s", win_path);
    } else {
        char test_path[256];
        snprintf(test_path, sizeof(test_path), "apps/%s", win_path);
        FILE *tf = fopen(test_path, "rb");
        if (tf) {
            fclose(tf);
            snprintf(out_host, out_sz, "%s", test_path);
        } else {
            snprintf(out_host, out_sz, "%s", win_path);
        }
    }
    /* Convert backslashes to forward slashes */
    for (size_t i = 0; i < strlen(out_host); i++) {
        if (out_host[i] == '\\') out_host[i] = '/';
    }
}

/* Memory Allocations */
static uint32_t s_heap_cursor = 0x02000000;

static uint32_t k32_VirtualAlloc(win32_process_t *proc, uint32_t *args) {
    uint32_t addr = args[0];
    uint32_t size = args[1];
    /* uint32_t alloc_type = args[2]; */
    /* uint32_t protect    = args[3]; */

    if (addr == 0) {
        addr = s_heap_cursor;
        s_heap_cursor += (size + 4095) & ~4095;
    }

    if (guest_mem_commit(&proc->mem, addr, size)) {
        LOGD("VirtualAlloc: addr=0x%08X, size=%u -> SUCCESS", addr, size);
        return addr;
    }
    LOGE("VirtualAlloc failed: addr=0x%08X, size=%u", addr, size);
    return 0;
}

static uint32_t k32_VirtualFree(win32_process_t *proc, uint32_t *args) {
    uint32_t addr = args[0];
    uint32_t size = args[1];
    /* uint32_t free_type = args[2]; */
    guest_mem_decommit(&proc->mem, addr, size);
    return 1;
}

static uint32_t k32_GetProcessHeap(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00010000; /* Process Heap Handle */
}

uint32_t k32_HeapAlloc(win32_process_t *proc, uint32_t *args) {
    /* uint32_t heap = args[0]; */
    uint32_t flags = args[1];
    uint32_t bytes = args[2];

    uint32_t addr = s_heap_cursor;
    uint32_t aligned = (bytes + 4095) & ~4095;
    s_heap_cursor += aligned;

    guest_mem_commit(&proc->mem, addr, bytes);
    uint8_t *p = guest_va_to_host(&proc->mem, addr);
    if (p && (flags & 0x00000008)) { /* HEAP_ZERO_MEMORY */
        memset(p, 0, bytes);
    }
    return addr;
}

static uint32_t k32_HeapFree(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GlobalAlloc(win32_process_t *proc, uint32_t *args) {
    return k32_HeapAlloc(proc, args);
}

static uint32_t k32_GlobalLock(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0]; /* Returns pointer itself */
}

static uint32_t k32_GlobalUnlock(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GlobalFree(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

/* File I/O */
static uint32_t k32_CreateFileA(win32_process_t *proc, uint32_t *args) {
    const char *win_path = (const char *)guest_va_to_host(&proc->mem, args[0]);
    uint32_t access = args[1];
    /* uint32_t share = args[2]; */
    uint32_t disp = args[4];

    if (!win_path) return 0xFFFFFFFF;

    char host_path[256];
    win32_translate_path(win_path, host_path, sizeof(host_path));

    const char *mode = "rb";
    if (access & 0x40000000) { /* GENERIC_WRITE */
        mode = (disp == 2) ? "wb" : "r+b";
    }

    FILE *f = fopen(host_path, mode);
    if (!f && (access & 0x40000000)) f = fopen(host_path, "wb");

    if (!f) {
        LOGW("CreateFileA: Cannot open %s (host: %s)", win_path, host_path);
        return 0xFFFFFFFF; /* INVALID_HANDLE_VALUE */
    }

    H_HANDLE h = alloc_file_handle(f);
    LOGI("CreateFileA: %s -> Handle 0x%08X", win_path, h);
    return (uint32_t)h;
}

static uint32_t k32_ReadFile(win32_process_t *proc, uint32_t *args) {
    H_HANDLE h = (H_HANDLE)args[0];
    uint32_t buf_va = args[1];
    uint32_t to_read = args[2];
    uint32_t bytes_read_va = args[3];

    FILE *f = get_file_handle(h);
    if (!f) return 0;

    uint8_t *dest = guest_va_to_host(&proc->mem, buf_va);
    if (!dest) return 0;

    size_t n = fread(dest, 1, to_read, f);
    if (bytes_read_va) {
        x86_mem_write32(&proc->cpu, bytes_read_va, (uint32_t)n);
    }
    return 1;
}

static uint32_t k32_WriteFile(win32_process_t *proc, uint32_t *args) {
    H_HANDLE h = (H_HANDLE)args[0];
    uint32_t buf_va = args[1];
    uint32_t to_write = args[2];
    uint32_t bytes_written_va = args[3];

    FILE *f = get_file_handle(h);
    if (!f) return 0;

    uint8_t *src = guest_va_to_host(&proc->mem, buf_va);
    if (!src) return 0;

    size_t n = fwrite(src, 1, to_write, f);
    if (bytes_written_va) {
        x86_mem_write32(&proc->cpu, bytes_written_va, (uint32_t)n);
    }
    return 1;
}

static uint32_t k32_CloseHandle(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = (H_HANDLE)args[0];
    free_file_handle(h);
    return 1;
}

static uint32_t k32_GetFileSize(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = (H_HANDLE)args[0];
    FILE *f = get_file_handle(h);
    if (!f) return 0xFFFFFFFF;

    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, cur, SEEK_SET);

    if (args[1]) {
        x86_mem_write32(&proc->cpu, args[1], 0); /* High 32-bit = 0 */
    }
    return (uint32_t)sz;
}

static uint32_t k32_SetFilePointer(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = (H_HANDLE)args[0];
    int32_t dist = (int32_t)args[1];
    uint32_t method = args[3];

    FILE *f = get_file_handle(h);
    if (!f) return 0xFFFFFFFF;

    int origin = SEEK_SET;
    if (method == 1) origin = SEEK_CUR;
    else if (method == 2) origin = SEEK_END;

    fseek(f, dist, origin);
    return (uint32_t)ftell(f);
}

/* Time & Timers */
static uint32_t k32_GetTickCount(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}

static uint32_t k32_Sleep(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t ms = args[0];
    sys_sleep_ms(ms);
    return 0;
}

/* Strings */
static uint32_t k32_lstrlenA(win32_process_t *proc, uint32_t *args) {
    const char *str = (const char *)guest_va_to_host(&proc->mem, args[0]);
    if (!str) return 0;
    return (uint32_t)strlen(str);
}

static uint32_t k32_lstrcpyA(win32_process_t *proc, uint32_t *args) {
    char *dest = (char *)guest_va_to_host(&proc->mem, args[0]);
    const char *src = (const char *)guest_va_to_host(&proc->mem, args[1]);
    if (dest && src) strcpy(dest, src);
    return args[0];
}

static uint32_t k32_lstrcatA(win32_process_t *proc, uint32_t *args) {
    char *dest = (char *)guest_va_to_host(&proc->mem, args[0]);
    const char *src = (const char *)guest_va_to_host(&proc->mem, args[1]);
    if (dest && src) strcat(dest, src);
    return args[0];
}

static uint32_t k32_lstrcmpA(win32_process_t *proc, uint32_t *args) {
    const char *s1 = (const char *)guest_va_to_host(&proc->mem, args[0]);
    const char *s2 = (const char *)guest_va_to_host(&proc->mem, args[1]);
    if (!s1 || !s2) return 0;
    return (uint32_t)strcmp(s1, s2);
}

/* System & Module */
static uint32_t k32_GetModuleHandleA(win32_process_t *proc, uint32_t *args) {
    const char *name = (const char *)guest_va_to_host(&proc->mem, args[0]);
    if (!name || name[0] == '\0') {
        return proc->image_base; /* NULL means current module */
    }
    return 0x10000000;
}

static uint32_t k32_GetCommandLineA(win32_process_t *proc, uint32_t *args) {
    (void)args;
    return proc->image_base; /* Pointer to command line string */
}

static uint32_t k32_GetCommandLineW(win32_process_t *proc, uint32_t *args) {
    (void)args;
    static const uint16_t cmdline[] = {'a','p','p','.','e','x','e',0};
    static uint32_t guest_cmdline_va = 0;
    if (!guest_cmdline_va) {
        guest_cmdline_va = 0x02800000;
        guest_mem_commit(&proc->mem, guest_cmdline_va, 0x1000);
        guest_mem_write(&proc->mem, guest_cmdline_va, cmdline, sizeof(cmdline));
    }
    return guest_cmdline_va;
}

static uint32_t k32_MulDiv(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int32_t nNumber = (int32_t)args[0];
    int32_t nNumerator = (int32_t)args[1];
    int32_t nDenominator = (int32_t)args[2];
    if (nDenominator == 0) return (uint32_t)-1;
    int64_t res = ((int64_t)nNumber * (int64_t)nNumerator) / (int64_t)nDenominator;
    return (uint32_t)(int32_t)res;
}

static uint32_t k32_GetVersionExA(win32_process_t *proc, uint32_t *args) {
    uint8_t *info = guest_va_to_host(&proc->mem, args[0]);
    if (!info) return 0;
    /* Simulate Windows 98 SE (4.10.2222) or Windows 2000 (5.0.2195) */
    *(uint32_t *)(info + 4) = 4;        /* dwMajorVersion = 4 */
    *(uint32_t *)(info + 8) = 10;       /* dwMinorVersion = 10 */
    *(uint32_t *)(info + 12) = 2222;    /* dwBuildNumber  = 2222 */
    *(uint32_t *)(info + 16) = 1;       /* dwPlatformId   = VER_PLATFORM_WIN32_WINDOWS */
    strcpy((char *)(info + 20), " A "); /* CSDVersion */
    return 1;
}

static uint32_t k32_ExitProcess(win32_process_t *proc, uint32_t *args) {
    proc->cpu.running = false;
    proc->cpu.exit_code = args[0];
    LOGI("Process exited cleanly with code: %u", args[0]);
    return 0;
}

static uint32_t k32_GetLastError(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* ERROR_SUCCESS */
}

static uint32_t k32_GetStartupInfoA(win32_process_t *proc, uint32_t *args) {
    uint32_t info_va = args[0];
    if (info_va) {
        uint8_t zero[68] = {0};
        guest_mem_write(&proc->mem, info_va, zero, sizeof(zero));
        uint32_t sz = 68;
        guest_mem_write(&proc->mem, info_va, &sz, 4);
    }
    return 0;
}

static uint32_t k32_GetPrivateProfileIntW(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t def_val = args[2];
    LOGI("GetPrivateProfileIntW -> %u", def_val);
    return def_val;
}

static uint32_t k32_GetPrivateProfileStringW(win32_process_t *proc, uint32_t *args) {
    uint32_t def_va = args[2];
    uint32_t out_va = args[3];
    uint32_t max_sz = args[4];
    if (def_va && out_va && max_sz > 0) {
        uint16_t wbuf[128] = {0};
        guest_mem_read(&proc->mem, def_va, wbuf, sizeof(wbuf));
        guest_mem_write(&proc->mem, out_va, wbuf, max_sz * 2);
    }
    return 0;
}

static uint32_t k32_GetPrivateProfileStringA(win32_process_t *proc, uint32_t *args) {
    const char *app = (const char *)guest_va_to_host(&proc->mem, args[0]);
    const char *key = (const char *)guest_va_to_host(&proc->mem, args[1]);
    printf("[DEBUG] GetPrivateProfileStringA: args[0]=0x%08X ('%s'), args[1]=0x%08X ('%s')\n",
           args[0], app ? app : "(null)", args[1], key ? key : "(null)");
    const char *def = (const char *)guest_va_to_host(&proc->mem, args[2]);
    char *out_buf   = (char *)guest_va_to_host(&proc->mem, args[3]);
    uint32_t out_sz = args[4];
    const char *file= (const char *)guest_va_to_host(&proc->mem, args[5]);

    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = '\0';

    const char *path = file ? file : "progman.ini";
    char local_path[256];
    if (strchr(path, '/') || strchr(path, '\\')) {
        snprintf(local_path, sizeof(local_path), "%s", path);
    } else {
        snprintf(local_path, sizeof(local_path), "apps/%s", path);
    }

    FILE *f = fopen(local_path, "r");
    if (!f && file) {
        f = fopen(file, "r");
    }

    bool in_section = false;
    bool found = false;
    char line[512];

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '[' && app) {
                char sec[128] = {0};
                char *end = strchr(p, ']');
                if (end) {
                    size_t slen = end - (p + 1);
                    if (slen >= sizeof(sec)) slen = sizeof(sec) - 1;
                    strncpy(sec, p + 1, slen);
                    sec[slen] = '\0';
                    in_section = (strcasecmp(sec, app) == 0);
                }
            } else if (in_section && key && *p != ';' && *p != '#' && *p != '\n' && *p != '\r') {
                char *eq = strchr(p, '=');
                if (eq) {
                    *eq = '\0';
                    char *k = p;
                    char *kend = eq - 1;
                    while (kend > k && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
                    if (strcasecmp(k, key) == 0) {
                        char *v = eq + 1;
                        while (*v == ' ' || *v == '\t') v++;
                        char *vend = v + strlen(v) - 1;
                        while (vend >= v && (*vend == '\r' || *vend == '\n' || *vend == ' ' || *vend == '\t')) *vend-- = '\0';
                        strncpy(out_buf, v, out_sz - 1);
                        out_buf[out_sz - 1] = '\0';
                        found = true;
                        break;
                    }
                }
            }
        }
        fclose(f);
    }

    if (!found) {
        if (def) {
            strncpy(out_buf, def, out_sz - 1);
            out_buf[out_sz - 1] = '\0';
        } else {
            out_buf[0] = '\0';
        }
    }

    LOGI("GetPrivateProfileStringA([%s], %s) -> \"%s\"", app ? app : "", key ? key : "", out_buf);
    return (uint32_t)strlen(out_buf);
}

static uint32_t k32_GetPrivateProfileIntA(win32_process_t *proc, uint32_t *args) {
    char buf[64] = {0};
    uint32_t def_val = args[2];
    uint32_t str_args[6] = { args[0], args[1], 0, 0, sizeof(buf), args[3] };
    char def_str[32];
    snprintf(def_str, sizeof(def_str), "%u", def_val);
    uint32_t def_va = 0x003FC000;
    guest_mem_commit(&proc->mem, def_va, 4096);
    guest_mem_write(&proc->mem, def_va, def_str, strlen(def_str) + 1);
    uint32_t out_va = def_va + 64;
    str_args[2] = def_va;
    str_args[3] = out_va;
    k32_GetPrivateProfileStringA(proc, str_args);
    guest_mem_read(&proc->mem, out_va, buf, sizeof(buf));
    return buf[0] ? (uint32_t)atoi(buf) : def_val;
}

static uint32_t k32_WritePrivateProfileStringA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static FILE *s_lfiles[32] = {0};

static uint32_t k32_lopen(win32_process_t *proc, uint32_t *args) {
    const char *path = (const char *)guest_va_to_host(&proc->mem, args[0]);
    if (!path) return (uint32_t)-1;
    char local_path[256];
    if (strchr(path, '/') || strchr(path, '\\')) {
        snprintf(local_path, sizeof(local_path), "%s", path);
    } else {
        snprintf(local_path, sizeof(local_path), "apps/%s", path);
    }
    FILE *f = fopen(local_path, "rb+");
    if (!f) f = fopen(local_path, "rb");
    if (!f) f = fopen(path, "rb");
    if (!f) return (uint32_t)-1;

    for (int i = 1; i < 32; i++) {
        if (!s_lfiles[i]) {
            s_lfiles[i] = f;
            return (uint32_t)i;
        }
    }
    fclose(f);
    return (uint32_t)-1;
}

static uint32_t k32_lcreat(win32_process_t *proc, uint32_t *args) {
    const char *path = (const char *)guest_va_to_host(&proc->mem, args[0]);
    if (!path) return (uint32_t)-1;
    FILE *f = fopen(path, "wb+");
    if (!f) return (uint32_t)-1;
    for (int i = 1; i < 32; i++) {
        if (!s_lfiles[i]) {
            s_lfiles[i] = f;
            return (uint32_t)i;
        }
    }
    fclose(f);
    return (uint32_t)-1;
}

static uint32_t k32_lread(win32_process_t *proc, uint32_t *args) {
    int fd = (int)args[0];
    uint32_t out_va = args[1];
    uint32_t bytes = args[2];
    if (fd <= 0 || fd >= 32 || !s_lfiles[fd] || !out_va) return (uint32_t)-1;
    void *buf = guest_va_to_host(&proc->mem, out_va);
    if (!buf) return (uint32_t)-1;
    return (uint32_t)fread(buf, 1, bytes, s_lfiles[fd]);
}

static uint32_t k32_lwrite(win32_process_t *proc, uint32_t *args) {
    int fd = (int)args[0];
    uint32_t in_va = args[1];
    uint32_t bytes = args[2];
    if (fd <= 0 || fd >= 32 || !s_lfiles[fd] || !in_va) return (uint32_t)-1;
    const void *buf = guest_va_to_host(&proc->mem, in_va);
    if (!buf) return (uint32_t)-1;
    return (uint32_t)fwrite(buf, 1, bytes, s_lfiles[fd]);
}

static uint32_t k32_llseek(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int fd = (int)args[0];
    long off = (long)args[1];
    int origin = (int)args[2];
    if (fd <= 0 || fd >= 32 || !s_lfiles[fd]) return (uint32_t)-1;
    fseek(s_lfiles[fd], off, origin);
    return (uint32_t)ftell(s_lfiles[fd]);
}

static uint32_t k32_lclose(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int fd = (int)args[0];
    if (fd > 0 && fd < 32 && s_lfiles[fd]) {
        fclose(s_lfiles[fd]);
        s_lfiles[fd] = NULL;
        return 0;
    }
    return (uint32_t)-1;
}

static uint32_t k32_OpenFile(win32_process_t *proc, uint32_t *args) {
    return k32_lopen(proc, args);
}

static uint32_t k32_WinExec(win32_process_t *proc, uint32_t *args) {
    const char *cmd = (const char *)guest_va_to_host(&proc->mem, args[0]);
    LOGI(">>> KERNEL32!WinExec: \"%s\" <<<", cmd ? cmd : "");
    if (cmd && cmd[0]) {
        char target[256];
        if (strchr(cmd, '/') || strchr(cmd, '\\')) {
            snprintf(target, sizeof(target), "%s", cmd);
        } else {
            snprintf(target, sizeof(target), "apps/%s", cmd);
        }
        win32_app_request_load(target);
        return 33;
    }
    return 0;
}

static uint32_t k32_LocalAlloc(win32_process_t *proc, uint32_t *args) {
    uint32_t flags = args[0];
    uint32_t bytes = args[1];
    uint32_t heap_args[3] = { 0, flags, bytes };
    return k32_HeapAlloc(proc, heap_args);
}
static uint32_t k32_LocalFree(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t k32_LocalLock(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}
static uint32_t k32_LocalReAlloc(win32_process_t *proc, uint32_t *args) {
    uint32_t old_va = args[0];
    uint32_t bytes = args[1];
    uint32_t flags = args[2];
    uint32_t heap_args[3] = { 0, flags, bytes };
    uint32_t new_va = k32_HeapAlloc(proc, heap_args);
    if (new_va && old_va) {
        uint8_t *old_p = guest_va_to_host(&proc->mem, old_va);
        uint8_t *new_p = guest_va_to_host(&proc->mem, new_va);
        if (old_p && new_p) memcpy(new_p, old_p, bytes);
    }
    return new_va;
}
static uint32_t k32_lstrcmpiA(win32_process_t *proc, uint32_t *args) {
    const char *s1 = (const char *)guest_va_to_host(&proc->mem, args[0]);
    const char *s2 = (const char *)guest_va_to_host(&proc->mem, args[1]);
    return (s1 && s2) ? (uint32_t)strcasecmp(s1, s2) : 0;
}
static uint32_t k32_lstrcpynA(win32_process_t *proc, uint32_t *args) {
    char *dst = (char *)guest_va_to_host(&proc->mem, args[0]);
    const char *src = (const char *)guest_va_to_host(&proc->mem, args[1]);
    int max_cnt = (int)args[2];
    if (dst && src && max_cnt > 0) {
        strncpy(dst, src, max_cnt - 1);
        dst[max_cnt - 1] = '\0';
    }
    return args[0];
}
static uint32_t k32_GetCurrentDirectoryA(win32_process_t *proc, uint32_t *args) {
    uint32_t max_len = args[0];
    char *out_buf = (char *)guest_va_to_host(&proc->mem, args[1]);
    if (out_buf && max_len > 0) {
        strcpy(out_buf, "C:\\");
        return (uint32_t)strlen(out_buf);
    }
    return 0;
}
static uint32_t k32_dummy_sync(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t k32_MultiByteToWideChar(win32_process_t *proc, uint32_t *args) {
    const char *lpMultiByteStr = (const char *)guest_va_to_host(&proc->mem, args[2]);
    int cbMultiByte = (int)args[3];
    uint32_t lpWideCharStr_va = args[4];
    int cchWideChar = (int)args[5];

    if (!lpMultiByteStr) return 0;
    int src_len = (cbMultiByte < 0) ? (int)strlen(lpMultiByteStr) + 1 : cbMultiByte;
    if (cchWideChar == 0 || !lpWideCharStr_va) return (uint32_t)src_len;

    int copy_len = src_len < cchWideChar ? src_len : cchWideChar;
    for (int i = 0; i < copy_len; i++) {
        uint16_t wch = (uint16_t)(uint8_t)lpMultiByteStr[i];
        guest_mem_write(&proc->mem, lpWideCharStr_va + i * 2, &wch, 2);
    }
    return (uint32_t)copy_len;
}

static uint32_t k32_WideCharToMultiByte(win32_process_t *proc, uint32_t *args) {
    const uint16_t *lpWideCharStr = (const uint16_t *)guest_va_to_host(&proc->mem, args[2]);
    int cchWideChar = (int)args[3];
    uint32_t lpMultiByteStr_va = args[4];
    int cbMultiByte = (int)args[5];

    if (!lpWideCharStr) return 0;
    int src_len = 0;
    if (cchWideChar < 0) {
        while (lpWideCharStr[src_len]) src_len++;
        src_len++;
    } else {
        src_len = cchWideChar;
    }
    if (cbMultiByte == 0 || !lpMultiByteStr_va) return (uint32_t)src_len;

    int copy_len = src_len < cbMultiByte ? src_len : cbMultiByte;
    for (int i = 0; i < copy_len; i++) {
        char ch = (char)(lpWideCharStr[i] & 0x7F);
        guest_mem_write(&proc->mem, lpMultiByteStr_va + i, &ch, 1);
    }
    return (uint32_t)copy_len;
}

static uint32_t k32_VirtualProtect(win32_process_t *proc, uint32_t *args) {
    uint32_t lpflOldProtect_va = args[3];
    if (lpflOldProtect_va) {
        uint32_t old_prot = 0x40; /* PAGE_EXECUTE_READWRITE */
        guest_mem_write(&proc->mem, lpflOldProtect_va, &old_prot, 4);
    }
    return 1;
}

static uint32_t k32_VirtualQuery(win32_process_t *proc, uint32_t *args) {
    uint32_t lpAddress = args[0];
    uint32_t lpBuffer_va = args[1];
    uint32_t dwLength = args[2];
    if (lpBuffer_va && dwLength >= 28) {
        struct {
            uint32_t BaseAddress;
            uint32_t AllocationBase;
            uint32_t AllocationProtect;
            uint32_t RegionSize;
            uint32_t State;
            uint32_t Protect;
            uint32_t Type;
        } mbi;
        mbi.BaseAddress = lpAddress & ~4095;
        mbi.AllocationBase = lpAddress & ~4095;
        mbi.AllocationProtect = 0x40;
        mbi.RegionSize = 65536;
        mbi.State = 0x1000; /* MEM_COMMIT */
        mbi.Protect = 0x40;
        mbi.Type = 0x20000; /* MEM_PRIVATE */
        guest_mem_write(&proc->mem, lpBuffer_va, &mbi, sizeof(mbi));
        return sizeof(mbi);
    }
    return 0;
}

static uint32_t k32_GetModuleFileNameA(win32_process_t *proc, uint32_t *args) {
    uint32_t buf_va = args[1];
    uint32_t max_sz = args[2];
    if (buf_va && max_sz > 0) {
        const char *name = "C:\\winmine.exe";
        guest_mem_write(&proc->mem, buf_va, name, strlen(name) + 1);
        return (uint32_t)strlen(name);
    }
    return 0;
}

static uint32_t k32_lstrlenW(win32_process_t *proc, uint32_t *args) {
    uint32_t str_va = args[0];
    if (!str_va) return 0;
    uint32_t len = 0;
    while (1) {
        uint16_t ch = 0;
        if (!guest_mem_read(&proc->mem, str_va + len * 2, &ch, 2) || ch == 0) break;
        len++;
    }
    return len;
}

static uint32_t k32_lstrcpyW(win32_process_t *proc, uint32_t *args) {
    uint32_t dst_va = args[0];
    uint32_t src_va = args[1];
    if (!dst_va || !src_va) return dst_va;
    uint32_t idx = 0;
    while (1) {
        uint16_t ch = 0;
        if (!guest_mem_read(&proc->mem, src_va + idx * 2, &ch, 2)) break;
        guest_mem_write(&proc->mem, dst_va + idx * 2, &ch, 2);
        if (ch == 0) break;
        idx++;
    }
    return dst_va;
}

static uint32_t k32_FindResourceW(win32_process_t *proc, uint32_t *args) {
    uint32_t lpName = args[1];
    uint32_t lpType = args[2];
    uint32_t name_id = (lpName < 0x10000) ? lpName : 0;
    uint32_t type_id = (lpType < 0x10000) ? lpType : 0;
    uint32_t sz = 0;
    uint32_t va = pe_find_resource_data(proc, type_id, name_id, &sz);
    LOGI("FindResourceW(type=%u, name=%u) -> VA 0x%08X (size %u)", type_id, name_id, va, sz);
    return va;
}

static uint32_t k32_LoadResource(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[1];
}

static uint32_t k32_LockResource(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}

static uint32_t k32_dummy_zero(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

void kernel32_init(void) {
    win32_register_api("KERNEL32.DLL", "VirtualProtect", 0, k32_VirtualProtect, 4);
    win32_register_api("KERNEL32.DLL", "VirtualQuery", 0, k32_VirtualQuery, 3);
    win32_register_api("KERNEL32.DLL", "MultiByteToWideChar", 0, k32_MultiByteToWideChar, 6);
    win32_register_api("KERNEL32.DLL", "WideCharToMultiByte", 0, k32_WideCharToMultiByte, 8);

    win32_register_api("KERNEL32.DLL", "VirtualAlloc", 0, k32_VirtualAlloc, 4);
    win32_register_api("KERNEL32.DLL", "VirtualFree", 0, k32_VirtualFree, 3);
    win32_register_api("KERNEL32.DLL", "GetProcessHeap", 0, k32_GetProcessHeap, 0);
    win32_register_api("KERNEL32.DLL", "HeapAlloc", 0, k32_HeapAlloc, 3);
    win32_register_api("KERNEL32.DLL", "HeapFree", 0, k32_HeapFree, 3);
    win32_register_api("KERNEL32.DLL", "GlobalAlloc", 0, k32_GlobalAlloc, 2);
    win32_register_api("KERNEL32.DLL", "GlobalLock", 0, k32_GlobalLock, 1);
    win32_register_api("KERNEL32.DLL", "GlobalUnlock", 0, k32_GlobalUnlock, 1);
    win32_register_api("KERNEL32.DLL", "GlobalFree", 0, k32_GlobalFree, 1);

    win32_register_api("KERNEL32.DLL", "CreateFileA", 0, k32_CreateFileA, 7);
    win32_register_api("KERNEL32.DLL", "ReadFile", 0, k32_ReadFile, 5);
    win32_register_api("KERNEL32.DLL", "WriteFile", 0, k32_WriteFile, 5);
    win32_register_api("KERNEL32.DLL", "CloseHandle", 0, k32_CloseHandle, 1);
    win32_register_api("KERNEL32.DLL", "GetFileSize", 0, k32_GetFileSize, 2);
    win32_register_api("KERNEL32.DLL", "SetFilePointer", 0, k32_SetFilePointer, 4);

    win32_register_api("KERNEL32.DLL", "GetTickCount", 0, k32_GetTickCount, 0);
    win32_register_api("KERNEL32.DLL", "Sleep", 0, k32_Sleep, 1);
    win32_register_api("KERNEL32.DLL", "lstrlenA", 0, k32_lstrlenA, 1);
    win32_register_api("KERNEL32.DLL", "lstrlenW", 0, k32_lstrlenW, 1);
    win32_register_api("KERNEL32.DLL", "lstrcpyA", 0, k32_lstrcpyA, 2);
    win32_register_api("KERNEL32.DLL", "lstrcpyW", 0, k32_lstrcpyW, 2);
    win32_register_api("KERNEL32.DLL", "lstrcatA", 0, k32_lstrcatA, 2);
    win32_register_api("KERNEL32.DLL", "lstrcmpA", 0, k32_lstrcmpA, 2);

    win32_register_api("KERNEL32.DLL", "GetModuleHandleA", 0, k32_GetModuleHandleA, 1);
    win32_register_api("KERNEL32.DLL", "GetStartupInfoA", 0, k32_GetStartupInfoA, 1);
    win32_register_api("KERNEL32.DLL", "GetCommandLineA", 0, k32_GetCommandLineA, 0);
    win32_register_api("KERNEL32.DLL", "GetCommandLineW", 0, k32_GetCommandLineW, 0);
    win32_register_api("KERNEL32.DLL", "MulDiv", 0, k32_MulDiv, 3);
    win32_register_api("KERNEL32.DLL", "GetVersionExA", 0, k32_GetVersionExA, 1);
    win32_register_api("KERNEL32.DLL", "GetModuleFileNameA", 0, k32_GetModuleFileNameA, 3);
    win32_register_api("KERNEL32.DLL", "GetPrivateProfileIntW", 0, k32_GetPrivateProfileIntW, 4);
    win32_register_api("KERNEL32.DLL", "GetPrivateProfileStringW", 0, k32_GetPrivateProfileStringW, 6);
    win32_register_api("KERNEL32.DLL", "GetPrivateProfileIntA", 0, k32_GetPrivateProfileIntA, 4);
    win32_register_api("KERNEL32.DLL", "GetPrivateProfileStringA", 0, k32_GetPrivateProfileStringA, 6);
    win32_register_api("KERNEL32.DLL", "WritePrivateProfileStringA", 0, k32_WritePrivateProfileStringA, 4);

    win32_register_api("KERNEL32.DLL", "LocalAlloc", 0, k32_LocalAlloc, 2);
    win32_register_api("KERNEL32.DLL", "LocalFree", 0, k32_LocalFree, 1);
    win32_register_api("KERNEL32.DLL", "LocalLock", 0, k32_LocalLock, 1);
    win32_register_api("KERNEL32.DLL", "LocalReAlloc", 0, k32_LocalReAlloc, 3);

    win32_register_api("KERNEL32.DLL", "_lopen", 0, k32_lopen, 2);
    win32_register_api("KERNEL32.DLL", "_lcreat", 0, k32_lcreat, 2);
    win32_register_api("KERNEL32.DLL", "_lread", 0, k32_lread, 3);
    win32_register_api("KERNEL32.DLL", "_lwrite", 0, k32_lwrite, 3);
    win32_register_api("KERNEL32.DLL", "_llseek", 0, k32_llseek, 3);
    win32_register_api("KERNEL32.DLL", "_lclose", 0, k32_lclose, 1);
    win32_register_api("KERNEL32.DLL", "OpenFile", 0, k32_OpenFile, 3);

    win32_register_api("KERNEL32.DLL", "WinExec", 0, k32_WinExec, 2);
    win32_register_api("KERNEL32.DLL", "GetCurrentDirectoryA", 0, k32_GetCurrentDirectoryA, 2);
    win32_register_api("KERNEL32.DLL", "lstrcmpiA", 0, k32_lstrcmpiA, 2);
    win32_register_api("KERNEL32.DLL", "lstrcpynA", 0, k32_lstrcpynA, 3);
    win32_register_api("KERNEL32.DLL", "GetModuleHandleW", 0, k32_GetModuleHandleA, 1);
    win32_register_api("KERNEL32.DLL", "FreeLibrary", 0, k32_dummy_zero, 1);

    win32_register_api("KERNEL32.DLL", "InitializeCriticalSection", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "EnterCriticalSection", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "LeaveCriticalSection", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "DeleteCriticalSection", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "TlsGetValue", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "SetUnhandledExceptionFilter", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "IsDBCSLeadByteEx", 0, k32_dummy_zero, 2);
    win32_register_api("KERNEL32.DLL", "FindResourceW", 0, k32_FindResourceW, 3);
    win32_register_api("KERNEL32.DLL", "LoadResource", 0, k32_LoadResource, 2);
    win32_register_api("KERNEL32.DLL", "LockResource", 0, k32_LockResource, 1);
    win32_register_api("KERNEL32.DLL", "GetProcAddress", 0, k32_dummy_zero, 2);
    win32_register_api("KERNEL32.DLL", "LoadLibraryA", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "OutputDebugStringA", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "ExitProcess", 0, k32_ExitProcess, 1);
    win32_register_api("KERNEL32.DLL", "GetLastError", 0, k32_GetLastError, 0);
}
