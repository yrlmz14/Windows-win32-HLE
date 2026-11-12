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

typedef struct {
    uint32_t magic;
    uint32_t size;
} heap_hdr_t;

uint32_t k32_HeapAlloc(win32_process_t *proc, uint32_t *args) {
    /* uint32_t heap = args[0]; */
    uint32_t flags = args[1];
    uint32_t bytes = args[2];
    uint32_t total = bytes + sizeof(heap_hdr_t);

    uint32_t addr = s_heap_cursor;
    uint32_t aligned = (total + 4095) & ~4095;
    s_heap_cursor += aligned;

    guest_mem_commit(&proc->mem, addr, total);
    heap_hdr_t *hdr = (heap_hdr_t *)guest_va_to_host(&proc->mem, addr);
    if (hdr) {
        hdr->magic = 0x48454150;
        hdr->size = bytes;
    }
    return addr + sizeof(heap_hdr_t);
}

static uint32_t k32_HeapFree(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GlobalAlloc(win32_process_t *proc, uint32_t *args) {
    uint32_t flags = (args[0] & 0x0040) ? 0x00000008 : 0; /* GMEM_ZEROINIT -> HEAP_ZERO_MEMORY */
    uint32_t bytes = args[1];
    uint32_t heap_args[3] = { 0, flags, bytes };
    return k32_HeapAlloc(proc, heap_args);
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
    static const char cmdline[] = "app.exe";
    uint32_t guest_cmdline_va = 0x02810000;
    guest_mem_commit(&proc->mem, guest_cmdline_va, 0x1000);
    guest_mem_write(&proc->mem, guest_cmdline_va, cmdline, sizeof(cmdline));
    return guest_cmdline_va;
}

static uint32_t k32_GetCommandLineW(win32_process_t *proc, uint32_t *args) {
    (void)args;
    static const uint16_t cmdline[] = {'a','p','p','.','e','x','e',0};
    uint32_t guest_cmdline_va = 0x02800000;
    guest_mem_commit(&proc->mem, guest_cmdline_va, 0x1000);
    guest_mem_write(&proc->mem, guest_cmdline_va, cmdline, sizeof(cmdline));
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
    uint32_t flags = (args[0] & 0x0040) ? 0x00000008 : 0; /* LMEM_ZEROINIT -> HEAP_ZERO_MEMORY */
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
static uint32_t k32_LocalSize(win32_process_t *proc, uint32_t *args);
static uint32_t k32_LocalReAlloc(win32_process_t *proc, uint32_t *args) {
    uint32_t old_va = args[0];
    uint32_t bytes = args[1];
    uint32_t flags = args[2];
    uint32_t heap_args[3] = { 0, flags, bytes };
    uint32_t new_va = k32_HeapAlloc(proc, heap_args);
    if (new_va && old_va) {
        uint32_t old_sz = k32_LocalSize(proc, args);
        uint32_t copy_sz = (old_sz < bytes) ? old_sz : bytes;
        uint32_t done = 0;
        while (done < copy_sz) {
            uint32_t chunk = (copy_sz - done > 4096) ? 4096 : (copy_sz - done);
            uint8_t tmp[4096];
            guest_mem_read(&proc->mem, old_va + done, tmp, chunk);
            guest_mem_write(&proc->mem, new_va + done, tmp, chunk);
            done += chunk;
        }
    }
    return new_va;
}
static uint32_t k32_LocalSize(win32_process_t *proc, uint32_t *args) {
    uint32_t addr = args[0];
    if (addr >= sizeof(heap_hdr_t)) {
        heap_hdr_t *hdr = (heap_hdr_t *)guest_va_to_host(&proc->mem, addr - sizeof(heap_hdr_t));
        if (hdr && hdr->magic == 0x48454150) {
            return hdr->size;
        }
    }
    return 4096;
}

static uint32_t k32_IsProcessorFeaturePresent(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t feat = args[0];
    switch (feat) {
        case 0: return 1; /* PF_FLOATING_POINT_PRECISION_ERRATA */
        case 1: return 1; /* PF_FLOATING_POINT_EMULATED */
        case 2: return 1; /* PF_COMPARE_EXCHANGE_DOUBLE */
        case 3: return 1; /* PF_MMX_INSTRUCTIONS_AVAILABLE */
        case 6: return 1; /* PF_PAE_ENABLED */
        case 10: return 1; /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
        case 12: return 1; /* PF_NX_ENABLED */
        case 13: return 1; /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
        case 14: return 1; /* PF_COMPARE_EXCHANGE128 */
        case 17: return 1; /* PF_XSAVE_ENABLED */
        case 23: return 1; /* PF_FASTFAIL_AVAILABLE */
        default: return 0;
    }
}

static uint32_t k32_pass_pointer(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}

static uint32_t k32_CoCreateGuid(win32_process_t *proc, uint32_t *args) {
    uint32_t guid_va = args[0];
    if (guid_va) {
        uint8_t dummy_guid[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };
        guest_mem_write(&proc->mem, guid_va, dummy_guid, 16);
    }
    return 0; /* S_OK */
}

static uint32_t k32_ResolveDelayLoadedAPI(win32_process_t *proc, uint32_t *args) {
    uint32_t iat_entry_va = args[4];
    uint32_t vector = 0;
    if (iat_entry_va && guest_mem_read(&proc->mem, iat_entry_va, &vector, 4) && vector != 0) {
        return vector;
    }
    return 0;
}

static uint32_t k32_GetDpiForWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 96; /* 96 DPI standard */
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

static uint32_t s_last_rsrc_size = 0;

static uint32_t k32_FindResourceA(win32_process_t *proc, uint32_t *args) {
    uint32_t lpName = args[1];
    uint32_t lpType = args[2];
    uint32_t type_id = (lpType < 0x10000) ? lpType : 0;
    uint32_t sz = 0;
    uint32_t va = pe_find_resource_ex(proc, type_id, lpName, false, &sz);
    if (va) s_last_rsrc_size = sz;
    LOGI("FindResourceA(type=%u, name=0x%X) -> VA 0x%08X (size %u)", type_id, lpName, va, sz);
    return va;
}

static uint32_t k32_FindResourceW(win32_process_t *proc, uint32_t *args) {
    uint32_t lpName = args[1];
    uint32_t lpType = args[2];
    uint32_t type_id = (lpType < 0x10000) ? lpType : 0;
    uint32_t sz = 0;
    uint32_t va = pe_find_resource_ex(proc, type_id, lpName, true, &sz);
    if (va) s_last_rsrc_size = sz;
    LOGI("FindResourceW(type=%u, name=0x%X) -> VA 0x%08X (size %u)", type_id, lpName, va, sz);
    return va;
}

static uint32_t k32_FindResourceExW(win32_process_t *proc, uint32_t *args) {
    uint32_t lpType = args[1];
    uint32_t lpName = args[2];
    uint32_t type_id = (lpType < 0x10000) ? lpType : 0;
    uint32_t sz = 0;
    uint32_t va = pe_find_resource_ex(proc, type_id, lpName, true, &sz);
    if (va) s_last_rsrc_size = sz;
    LOGI("FindResourceExW(type=%u, name=0x%X) -> VA 0x%08X (size %u)", type_id, lpName, va, sz);
    return va;
}

static uint32_t k32_SizeofResource(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return s_last_rsrc_size ? s_last_rsrc_size : 4096;
}

bool win32_is_api_implemented(int api_id);

static uint32_t k32_GetProcAddress(win32_process_t *proc, uint32_t *args) {
    uint32_t lpProcName = args[1];
    if (!lpProcName) return 0;
    char func_name[128] = {0};
    uint16_t ordinal = 0;
    if ((lpProcName >> 16) == 0) {
        ordinal = (uint16_t)(lpProcName & 0xFFFF);
    } else {
        for (int k = 0; k < 127; k++) {
            uint8_t ch = 0;
            if (!guest_mem_read(&proc->mem, lpProcName + k, &ch, 1) || ch == 0) break;
            func_name[k] = (char)ch;
        }
    }
    int api_id = win32_register_or_find_api("DYNAMIC.DLL", func_name[0] ? func_name : NULL, ordinal);
    if (!win32_is_api_implemented(api_id)) {
        LOGI("GetProcAddress(\"%s\", ord=%u) -> NULL (optional API not present)", func_name, ordinal);
        return 0;
    }
    uint32_t vec = HLE_API_BASE | (uint32_t)(api_id & 0xFFFF);
    LOGI("GetProcAddress(\"%s\", ord=%u) -> 0x%08X", func_name, ordinal, vec);
    return vec;
}

static uint32_t k32_LoadLibraryAny(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x10000000;
}

static uint32_t k32_LoadResource(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[1];
}

static uint32_t k32_LockResource(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[0];
}

static uint32_t k32_WindowsCreateStringReference(win32_process_t *proc, uint32_t *args) {
    uint32_t src_str = args[0];
    uint32_t out_hstr = args[3];
    if (out_hstr) {
        guest_mem_write(&proc->mem, out_hstr, &src_str, 4);
    }
    return 0; /* S_OK */
}

static uint32_t k32_WindowsDeleteString(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* S_OK */
}

static uint32_t k32_RoGetActivationFactory(win32_process_t *proc, uint32_t *args) {
    uint32_t out_factory = args[2];
    if (out_factory) {
        uint32_t null_ptr = 0;
        guest_mem_write(&proc->mem, out_factory, &null_ptr, 4);
    }
    return 0x80004002; /* E_NOINTERFACE / REGDB_E_CLASSNOTREG */
}

static uint32_t k32_CoCreateInstance(win32_process_t *proc, uint32_t *args) {
    uint32_t out_ppv = args[4];
    if (out_ppv) {
        uint32_t null_ptr = 0;
        guest_mem_write(&proc->mem, out_ppv, &null_ptr, 4);
    }
    return 0x80004002; /* E_NOINTERFACE */
}

static uint32_t k32_GetCurrentThreadId(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GetCurrentProcessId(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1000;
}

static uint32_t k32_GetCurrentProcess(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0xFFFFFFFF;
}

static uint32_t k32_OpenProcessToken(win32_process_t *proc, uint32_t *args) {
    uint32_t token_ptr = args[2];
    if (token_ptr) {
        uint32_t tok = 0x00010002;
        guest_mem_write(&proc->mem, token_ptr, &tok, 4);
    }
    return 1;
}

static uint32_t k32_IsDebuggerPresent(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t k32_FormatMessageW(win32_process_t *proc, uint32_t *args) {
    uint32_t dwFlags = args[0];
    uint32_t lpSource = args[1];
    uint32_t lpBuffer = args[4];
    uint32_t nSize = args[5];
    uint32_t arguments_va = args[6];

    uint16_t out_wbuf[512] = {0};
    uint32_t out_len = 0;

    if ((dwFlags & 0x0400 /* FORMAT_MESSAGE_FROM_STRING */) && lpSource != 0) {
        uint16_t fmt[256] = {0};
        guest_mem_read(&proc->mem, lpSource, fmt, sizeof(fmt) - 2);
        int fi = 0;
        while (fmt[fi] != 0 && out_len < 510) {
            if (fmt[fi] == '%' && fmt[fi + 1] >= '1' && fmt[fi + 1] <= '9') {
                int arg_idx = (fmt[fi + 1] - '1');
                fi += 2;
                /* Skip optional !format! specifier if present */
                if (fmt[fi] == '!') {
                    fi++;
                    while (fmt[fi] != 0 && fmt[fi] != '!') fi++;
                    if (fmt[fi] == '!') fi++;
                }
                if (!(dwFlags & 0x0200 /* FORMAT_MESSAGE_IGNORE_INSERTS */) && arguments_va != 0) {
                    uint32_t str_ptr = 0;
                    guest_mem_read(&proc->mem, arguments_va + arg_idx * 4, &str_ptr, 4);
                    if (str_ptr != 0) {
                        uint16_t ins[256] = {0};
                        guest_mem_read(&proc->mem, str_ptr, ins, sizeof(ins) - 2);
                        int ii = 0;
                        while (ins[ii] != 0 && out_len < 510) {
                            out_wbuf[out_len++] = ins[ii++];
                        }
                    }
                }
            } else if (fmt[fi] == '%' && fmt[fi + 1] == '0') {
                /* %0 terminates output without newline */
                break;
            } else if (fmt[fi] == '%' && fmt[fi + 1] != 0) {
                fi++;
                out_wbuf[out_len++] = fmt[fi++];
            } else {
                out_wbuf[out_len++] = fmt[fi++];
            }
        }
        out_wbuf[out_len] = 0;
    } else {
        static const uint16_t s_fallback[] = { 'O','K',0 };
        memcpy(out_wbuf, s_fallback, sizeof(s_fallback));
        out_len = 2;
    }

    if (dwFlags & 0x0100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t alloc_args[2] = { 0x0040, (out_len + 1) * 2 };
        uint32_t ptr = k32_LocalAlloc(proc, alloc_args);
        if (ptr && lpBuffer) {
            guest_mem_write(&proc->mem, ptr, out_wbuf, (out_len + 1) * 2);
            guest_mem_write(&proc->mem, lpBuffer, &ptr, 4);
            return out_len;
        }
    } else if (lpBuffer && (nSize == 0 || nSize > out_len)) {
        guest_mem_write(&proc->mem, lpBuffer, out_wbuf, (out_len + 1) * 2);
        return out_len;
    }
    return out_len;
}

static uint32_t k32_GetDpiForMonitor(win32_process_t *proc, uint32_t *args) {
    uint32_t dpiX_va = args[2];
    uint32_t dpiY_va = args[3];
    uint32_t dpi = 96;
    if (dpiX_va) guest_mem_write(&proc->mem, dpiX_va, &dpi, 4);
    if (dpiY_va) guest_mem_write(&proc->mem, dpiY_va, &dpi, 4);
    return 0; /* S_OK */
}

static uint32_t k32_lstrcmpiW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t k32_GetVersion(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0xC0000A04U; /* Windows 98 (4.10) */
}

static uint32_t k32_GetACP(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1252;
}

static uint32_t k32_GetOEMCP(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 437;
}

static uint32_t k32_GetCPInfo(win32_process_t *proc, uint32_t *args) {
    uint32_t cpinfo_va = args[1];
    if (cpinfo_va) {
        uint8_t info[20] = {0};
        uint32_t max_char_sz = 1;
        memcpy(info, &max_char_sz, 4);
        info[4] = '?';
        guest_mem_write(&proc->mem, cpinfo_va, info, sizeof(info));
    }
    return 1;
}

static uint32_t k32_GetStdHandle(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t nStdHandle = args[0];
    if (nStdHandle == (uint32_t)-10) return 0x00000001; /* STD_INPUT_HANDLE */
    if (nStdHandle == (uint32_t)-11) return 0x00000002; /* STD_OUTPUT_HANDLE */
    if (nStdHandle == (uint32_t)-12) return 0x00000003; /* STD_ERROR_HANDLE */
    return 0xFFFFFFFF;
}

static uint32_t k32_GetFileType(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t h = args[0];
    if (h >= 1 && h <= 3) return 2; /* FILE_TYPE_CHAR */
    return 1; /* FILE_TYPE_DISK */
}

static uint32_t k32_GetEnvironmentStrings(win32_process_t *proc, uint32_t *args) {
    (void)args;
    static const char s_env[] = "PATH=C:\\\0WINDIR=C:\\\0\0";
    uint32_t s_env_va = 0x02802000;
    guest_mem_commit(&proc->mem, s_env_va, 0x1000);
    guest_mem_write(&proc->mem, s_env_va, s_env, sizeof(s_env));
    return s_env_va;
}

static uint32_t k32_GetEnvironmentStringsW(win32_process_t *proc, uint32_t *args) {
    (void)args;
    static const uint16_t s_wenv[] = { 'P','A','T','H','=','C',':','\\',0,0 };
    uint32_t s_wenv_va = 0x02803000;
    guest_mem_commit(&proc->mem, s_wenv_va, 0x1000);
    guest_mem_write(&proc->mem, s_wenv_va, s_wenv, sizeof(s_wenv));
    return s_wenv_va;
}

static uint32_t k32_FreeEnvironmentStrings(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GetProfileIntA(win32_process_t *proc, uint32_t *args) {
    uint32_t priv_args[4] = { args[0], args[1], args[2], 0 };
    return k32_GetPrivateProfileIntA(proc, priv_args);
}

static uint32_t k32_GetProfileStringA(win32_process_t *proc, uint32_t *args) {
    uint32_t priv_args[6] = { args[0], args[1], args[2], args[3], args[4], 0 };
    return k32_GetPrivateProfileStringA(proc, priv_args);
}

static uint32_t k32_WriteProfileStringA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t k32_GlobalCompact(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x01000000;
}

static uint32_t k32_GetLocaleInfoA(win32_process_t *proc, uint32_t *args) {
    /* uint32_t locale = args[0]; */
    /* uint32_t lctype = args[1]; */
    uint32_t buf_va = args[2];
    int cchData = (int)args[3];
    if (buf_va && cchData > 0) {
        const char *val = ".";
        guest_mem_write(&proc->mem, buf_va, val, 2);
        return 2;
    }
    return 2;
}

static uint32_t k32_GetLocalTime(win32_process_t *proc, uint32_t *args) {
    uint32_t st_va = args[0];
    if (st_va) {
        uint16_t st[8] = { 2026, 11, 4, 12, 12, 0, 0, 0 };
        guest_mem_write(&proc->mem, st_va, st, sizeof(st));
    }
    return 0;
}

static uint32_t k32_GetTimeFormatA(win32_process_t *proc, uint32_t *args) {
    uint32_t buf_va = args[4];
    int cch = (int)args[5];
    const char *str = "12:00 PM";
    if (buf_va && cch > 0) guest_mem_write(&proc->mem, buf_va, str, strlen(str) + 1);
    return (uint32_t)strlen(str) + 1;
}

static uint32_t k32_GetDateFormatA(win32_process_t *proc, uint32_t *args) {
    uint32_t buf_va = args[4];
    int cch = (int)args[5];
    const char *str = "11/12/2026";
    if (buf_va && cch > 0) guest_mem_write(&proc->mem, buf_va, str, strlen(str) + 1);
    return (uint32_t)strlen(str) + 1;
}

static uint32_t k32_FindFirstFileA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0xFFFFFFFFU; /* INVALID_HANDLE_VALUE */
}

static uint32_t k32_dummy_zero(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

void kernel32_init(void) {
    s_heap_cursor = 0x02000000;
    s_last_rsrc_size = 0;
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (s_handles[i]) { fclose(s_handles[i]); s_handles[i] = NULL; }
    }
    for (int i = 0; i < 32; i++) {
        if (s_lfiles[i]) { fclose(s_lfiles[i]); s_lfiles[i] = NULL; }
    }

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
    win32_register_api("KERNEL32.DLL", "GlobalReAlloc", 0, k32_LocalReAlloc, 3);
    win32_register_api("KERNEL32.DLL", "GlobalSize", 0, k32_LocalSize, 1);
    win32_register_api("KERNEL32.DLL", "GlobalCompact", 0, k32_GlobalCompact, 1);
    win32_register_api("KERNEL32.DLL", "GlobalLock", 0, k32_GlobalLock, 1);
    win32_register_api("KERNEL32.DLL", "GlobalUnlock", 0, k32_GlobalUnlock, 1);
    win32_register_api("KERNEL32.DLL", "GlobalFree", 0, k32_GlobalFree, 1);
    win32_register_api("KERNEL32.DLL", "GetVersion", 0, k32_GetVersion, 0);
    win32_register_api("KERNEL32.DLL", "GetACP", 0, k32_GetACP, 0);
    win32_register_api("KERNEL32.DLL", "GetOEMCP", 0, k32_GetOEMCP, 0);
    win32_register_api("KERNEL32.DLL", "GetCPInfo", 0, k32_GetCPInfo, 2);
    win32_register_api("KERNEL32.DLL", "GetStdHandle", 0, k32_GetStdHandle, 1);
    win32_register_api("KERNEL32.DLL", "GetFileType", 0, k32_GetFileType, 1);
    win32_register_api("KERNEL32.DLL", "GetEnvironmentStrings", 0, k32_GetEnvironmentStrings, 0);
    win32_register_api("KERNEL32.DLL", "GetEnvironmentStringsA", 0, k32_GetEnvironmentStrings, 0);
    win32_register_api("KERNEL32.DLL", "GetEnvironmentStringsW", 0, k32_GetEnvironmentStringsW, 0);
    win32_register_api("KERNEL32.DLL", "FreeEnvironmentStringsA", 0, k32_FreeEnvironmentStrings, 1);
    win32_register_api("KERNEL32.DLL", "FreeEnvironmentStringsW", 0, k32_FreeEnvironmentStrings, 1);
    win32_register_api("KERNEL32.DLL", "GetProfileIntA", 0, k32_GetProfileIntA, 3);
    win32_register_api("KERNEL32.DLL", "GetProfileStringA", 0, k32_GetProfileStringA, 5);
    win32_register_api("KERNEL32.DLL", "WriteProfileStringA", 0, k32_WriteProfileStringA, 3);
    win32_register_api("KERNEL32.DLL", "UnhandledExceptionFilter", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "RaiseException", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "RtlUnwind", 0, k32_dummy_zero, 4);

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
    win32_register_api("KERNEL32.DLL", "GetStartupInfoW", 0, k32_GetStartupInfoA, 1);
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

    win32_register_api("KERNEL32.DLL", "CoTaskMemAlloc", 0, k32_LocalAlloc, 1);
    win32_register_api("KERNEL32.DLL", "CoTaskMemFree", 0, k32_LocalFree, 1);
    win32_register_api("KERNEL32.DLL", "LocalAlloc", 0, k32_LocalAlloc, 2);
    win32_register_api("KERNEL32.DLL", "LocalFree", 0, k32_LocalFree, 1);
    win32_register_api("KERNEL32.DLL", "LocalLock", 0, k32_LocalLock, 1);
    win32_register_api("KERNEL32.DLL", "LocalUnlock", 0, k32_GlobalUnlock, 1);
    win32_register_api("KERNEL32.DLL", "LocalReAlloc", 0, k32_LocalReAlloc, 3);
    win32_register_api("KERNEL32.DLL", "LocalSize", 0, k32_LocalSize, 1);
    win32_register_api("KERNEL32.DLL", "GetLocaleInfoA", 0, k32_GetLocaleInfoA, 4);
    win32_register_api("KERNEL32.DLL", "GetLocaleInfoW", 0, k32_GetLocaleInfoA, 4);
    win32_register_api("KERNEL32.DLL", "GetLocalTime", 0, k32_GetLocalTime, 1);
    win32_register_api("KERNEL32.DLL", "GetSystemTime", 0, k32_GetLocalTime, 1);
    win32_register_api("KERNEL32.DLL", "GetTimeFormatA", 0, k32_GetTimeFormatA, 6);
    win32_register_api("KERNEL32.DLL", "GetDateFormatA", 0, k32_GetDateFormatA, 6);
    win32_register_api("KERNEL32.DLL", "FindFirstFileA", 0, k32_FindFirstFileA, 2);
    win32_register_api("KERNEL32.DLL", "FindClose", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "DeleteFileA", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "IsProcessorFeaturePresent", 0, k32_IsProcessorFeaturePresent, 1);
    win32_register_api("KERNEL32.DLL", "HeapSetInformation", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "CoInitializeEx", 0, k32_dummy_zero, 2);
    win32_register_api("KERNEL32.DLL", "CoUninitialize", 0, k32_dummy_zero, 0);
    win32_register_api("KERNEL32.DLL", "CoCreateGuid", 0, k32_CoCreateGuid, 1);
    win32_register_api("KERNEL32.DLL", "CoCreateInstance", 0, k32_CoCreateInstance, 5);
    win32_register_api("KERNEL32.DLL", "GetCurrentThreadId", 0, k32_GetCurrentThreadId, 0);
    win32_register_api("KERNEL32.DLL", "GetCurrentProcessId", 0, k32_GetCurrentProcessId, 0);
    win32_register_api("KERNEL32.DLL", "GetCurrentProcess", 0, k32_GetCurrentProcess, 0);
    win32_register_api("KERNEL32.DLL", "OpenProcessToken", 0, k32_OpenProcessToken, 3);
    win32_register_api("KERNEL32.DLL", "IsDebuggerPresent", 0, k32_IsDebuggerPresent, 0);
    win32_register_api("KERNEL32.DLL", "EventRegister", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "EventUnregister", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "EventWrite", 0, k32_dummy_zero, 5);
    win32_register_api("KERNEL32.DLL", "EncodePointer", 0, k32_pass_pointer, 1);
    win32_register_api("KERNEL32.DLL", "DecodePointer", 0, k32_pass_pointer, 1);
    win32_register_api("KERNEL32.DLL", "InitializeCriticalSectionAndSpinCount", 0, k32_dummy_sync, 2);
    win32_register_api("KERNEL32.DLL", "InitializeCriticalSectionEx", 0, k32_dummy_sync, 3);
    win32_register_api("KERNEL32.DLL", "AcquireSRWLockExclusive", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "ReleaseSRWLockExclusive", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "AcquireSRWLockShared", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "ReleaseSRWLockShared", 0, k32_dummy_sync, 1);
    win32_register_api("KERNEL32.DLL", "CreateSemaphoreExW", 0, k32_dummy_zero, 6);
    win32_register_api("KERNEL32.DLL", "ReleaseSemaphore", 0, k32_dummy_zero, 3);
    win32_register_api("KERNEL32.DLL", "CreateMutexExW", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "ReleaseMutex", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "CreateEventExW", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "CreateEventW", 0, k32_dummy_zero, 4);
    win32_register_api("KERNEL32.DLL", "SetEvent", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "ResetEvent", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "WaitForSingleObject", 0, k32_dummy_zero, 2);
    win32_register_api("KERNEL32.DLL", "WaitForSingleObjectEx", 0, k32_dummy_zero, 3);
    win32_register_api("KERNEL32.DLL", "GetModuleHandleExW", 0, k32_GetModuleHandleA, 3);
    win32_register_api("KERNEL32.DLL", "GetModuleFileNameW", 0, k32_GetModuleFileNameA, 3);
    win32_register_api("KERNEL32.DLL", "FindResourceA", 0, k32_FindResourceA, 3);
    win32_register_api("KERNEL32.DLL", "FindResourceExW", 0, k32_FindResourceExW, 4);
    win32_register_api("KERNEL32.DLL", "SizeofResource", 0, k32_SizeofResource, 2);
    win32_register_api("KERNEL32.DLL", "FormatMessageW", 0, k32_FormatMessageW, 7);
    win32_register_api("KERNEL32.DLL", "GetDpiForMonitor", 0, k32_GetDpiForMonitor, 4);
    win32_register_api("KERNEL32.DLL", "lstrcmpiW", 0, k32_lstrcmpiW, 2);
    win32_register_api("KERNEL32.DLL", "ResolveDelayLoadedAPI", 0, k32_ResolveDelayLoadedAPI, 6);
    win32_register_api("KERNEL32.DLL", "SetThreadDpiAwarenessContext", 0, k32_pass_pointer, 1);
    win32_register_api("KERNEL32.DLL", "GetDpiForWindow", 0, k32_GetDpiForWindow, 1);
    win32_register_api("KERNEL32.DLL", "SystemParametersInfoForDpi", 0, k32_dummy_zero, 5);
    win32_register_api("KERNEL32.DLL", "WindowsCreateStringReference", 0, k32_WindowsCreateStringReference, 4);
    win32_register_api("KERNEL32.DLL", "WindowsDeleteString", 0, k32_WindowsDeleteString, 1);
    win32_register_api("KERNEL32.DLL", "RoGetActivationFactory", 0, k32_RoGetActivationFactory, 3);
    win32_register_api("KERNEL32.DLL", "RegisterApplicationRestart", 0, k32_dummy_zero, 2);

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
    win32_register_api("KERNEL32.DLL", "GetProcAddress", 0, k32_GetProcAddress, 2);
    win32_register_api("KERNEL32.DLL", "LoadLibraryA", 0, k32_LoadLibraryAny, 1);
    win32_register_api("KERNEL32.DLL", "LoadLibraryW", 0, k32_LoadLibraryAny, 1);
    win32_register_api("KERNEL32.DLL", "LoadLibraryExA", 0, k32_LoadLibraryAny, 3);
    win32_register_api("KERNEL32.DLL", "LoadLibraryExW", 0, k32_LoadLibraryAny, 3);
    win32_register_api("KERNEL32.DLL", "OutputDebugStringA", 0, k32_dummy_zero, 1);
    win32_register_api("KERNEL32.DLL", "ExitProcess", 0, k32_ExitProcess, 1);
    win32_register_api("KERNEL32.DLL", "GetLastError", 0, k32_GetLastError, 0);
}
