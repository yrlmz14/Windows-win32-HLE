#include "hle.h"
#include "memory.h"
#include "teb_peb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

// Process & Lifecycle
static uint32_t hle_ExitProcess(struct CPUState* cpu, uint32_t* args) {
    DWORD exit_code = args[0];
    extern void cpu_halt(struct CPUState* cpu, uint32_t exit_code);
    cpu_halt(cpu, exit_code);
    return 0;
}

static uint32_t hle_GetCurrentProcess(struct CPUState* cpu, uint32_t* args) {
    return (uint32_t)-1;
}

static uint32_t hle_GetCurrentThread(struct CPUState* cpu, uint32_t* args) {
    return (uint32_t)-2;
}

static uint32_t hle_GetCurrentProcessId(struct CPUState* cpu, uint32_t* args) {
    return 1000;
}

static uint32_t hle_GetCurrentThreadId(struct CPUState* cpu, uint32_t* args) {
    return 1001;
}

// Console & Standard Handles
static uint32_t hle_GetStdHandle(struct CPUState* cpu, uint32_t* args) {
    DWORD nStdHandle = args[0];
    if (nStdHandle == (DWORD)-10) return (uint32_t)-10; // stdin
    if (nStdHandle == (DWORD)-11) return (uint32_t)-11; // stdout
    if (nStdHandle == (DWORD)-12) return (uint32_t)-12; // stderr
    return (uint32_t)INVALID_HANDLE_VALUE;
}

static uint32_t hle_SetStdHandle(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

// File & I/O
static uint32_t hle_WriteFile(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFile = (HANDLE)args[0];
    DWORD lpBuffer = args[1];
    DWORD nNumberOfBytesToWrite = args[2];
    DWORD lpNumberOfBytesWritten = args[3];
    DWORD lpOverlapped = args[4];

    void* native_fd = handle_get(hFile, HANDLE_TYPE_FILE);
    if (!native_fd && hFile != (HANDLE)(uint32_t)-10 && hFile != (HANDLE)(uint32_t)-11 && hFile != (HANDLE)(uint32_t)-12) {
        hle_set_last_error(6); // ERROR_INVALID_HANDLE
        return 0;
    }

    int fd = (int)(intptr_t)native_fd;
    if (hFile == (HANDLE)(uint32_t)-11) fd = 1;
    else if (hFile == (HANDLE)(uint32_t)-12) fd = 2;
    else if (hFile == (HANDLE)(uint32_t)-10) fd = 0;

    void* buf = mem_to_host(lpBuffer);
    if (!buf) {
        hle_set_last_error(998); // ERROR_NOACCESS
        return 0;
    }

    ssize_t written = write(fd, buf, nNumberOfBytesToWrite);
    if (written < 0) {
        hle_set_last_error(5);
        return 0;
    }

    if (lpNumberOfBytesWritten) {
        mem_write_u32(lpNumberOfBytesWritten, (DWORD)written);
    }
    return 1;
}

static uint32_t hle_ReadFile(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFile = (HANDLE)args[0];
    DWORD lpBuffer = args[1];
    DWORD nNumberOfBytesToRead = args[2];
    DWORD lpNumberOfBytesRead = args[3];
    DWORD lpOverlapped = args[4];

    void* native_fd = handle_get(hFile, HANDLE_TYPE_FILE);
    int fd = (int)(intptr_t)native_fd;
    if (hFile == (HANDLE)(uint32_t)-10) fd = 0;

    void* buf = mem_to_host(lpBuffer);
    if (!buf) return 0;

    ssize_t bytes_read = read(fd, buf, nNumberOfBytesToRead);
    if (bytes_read < 0) return 0;

    if (lpNumberOfBytesRead) {
        mem_write_u32(lpNumberOfBytesRead, (DWORD)bytes_read);
    }
    return 1;
}

static uint32_t hle_WriteConsoleA(struct CPUState* cpu, uint32_t* args) {
    // args: hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved
    return hle_WriteFile(cpu, args);
}

static uint32_t hle_WriteConsoleW(struct CPUState* cpu, uint32_t* args) {
    DWORD lpBuffer = args[1];
    DWORD nChars = args[2];
    DWORD lpWritten = args[3];

    // Convert UTF-16 to UTF-8
    char* utf8 = (char*)malloc(nChars * 4 + 1);
    size_t out_pos = 0;
    for (DWORD i = 0; i < nChars; i++) {
        uint16_t wc = mem_read_u16(lpBuffer + i * 2);
        if (wc < 0x80) {
            utf8[out_pos++] = (char)wc;
        } else if (wc < 0x800) {
            utf8[out_pos++] = 0xC0 | (wc >> 6);
            utf8[out_pos++] = 0x80 | (wc & 0x3F);
        } else {
            utf8[out_pos++] = 0xE0 | (wc >> 12);
            utf8[out_pos++] = 0x80 | ((wc >> 6) & 0x3F);
            utf8[out_pos++] = 0x80 | (wc & 0x3F);
        }
    }
    utf8[out_pos] = 0;

    write(1, utf8, out_pos);
    free(utf8);

    if (lpWritten) mem_write_u32(lpWritten, nChars);
    return 1;
}

// File Creation & Management
uint32_t hle_CreateFileW_impl(DWORD name_ptr, DWORD access, DWORD share, DWORD disp) {
    if (!name_ptr) return (uint32_t)INVALID_HANDLE_VALUE;

    char path[512] = {0};
    int i = 0;
    while (i < 510) {
        uint16_t ch = mem_read_u16(name_ptr + i * 2);
        if (ch == 0) break;
        if (ch == '\\') path[i] = '/';
        else path[i] = (char)ch;
        i++;
    }
    path[i] = 0;

    int flags = 0;
    if ((access & 0x80000000) && (access & 0x40000000)) flags = O_RDWR;
    else if (access & 0x40000000) flags = O_WRONLY;
    else flags = O_RDONLY;

    if (disp == 2) flags |= O_CREAT | O_TRUNC;
    else if (disp == 1) flags |= O_CREAT | O_EXCL;
    else if (disp == 4) flags |= O_CREAT;

    int fd = open(path, flags, 0666);
    if (fd < 0) {
        hle_set_last_error(2); // ERROR_FILE_NOT_FOUND
        return (uint32_t)INVALID_HANDLE_VALUE;
    }

    return (uint32_t)handle_alloc((void*)(intptr_t)fd, HANDLE_TYPE_FILE);
}

static uint32_t hle_CreateFileW(struct CPUState* cpu, uint32_t* args) {
    // args: lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile
    return hle_CreateFileW_impl(args[0], args[1], args[2], args[4]);
}

static uint32_t hle_CreateFileA(struct CPUState* cpu, uint32_t* args) {
    const char* str = (const char*)mem_to_host(args[0]);
    if (!str) return (uint32_t)INVALID_HANDLE_VALUE;

    // Convert ASCII to UTF-16 in scratch space and call W impl
    WCHAR wpath[512] = {0};
    for (int i = 0; str[i] && i < 510; i++) wpath[i] = (WCHAR)(uint8_t)str[i];
    DWORD scratch = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    mem_write(scratch, wpath, sizeof(wpath));

    uint32_t res = hle_CreateFileW_impl(scratch, args[1], args[2], args[4]);
    mem_virtual_free(scratch, 0, MEM_RELEASE);
    return res;
}

static uint32_t hle_DeleteFileW(struct CPUState* cpu, uint32_t* args) {
    DWORD name_ptr = args[0];
    if (!name_ptr) return 0;
    char path[512] = {0};
    for (int i = 0; i < 510; i++) {
        uint16_t ch = mem_read_u16(name_ptr + i * 2);
        if (!ch) break;
        path[i] = (ch == '\\') ? '/' : (char)ch;
    }
    return (unlink(path) == 0) ? 1 : 0;
}

static uint32_t hle_GetFileAttributesW(struct CPUState* cpu, uint32_t* args) {
    return 0x80; // FILE_ATTRIBUTE_NORMAL
}

static uint32_t hle_GetFileAttributesA(struct CPUState* cpu, uint32_t* args) {
    const char* path = (const char*)mem_to_host(args[0]);
    if (!path) return 0xFFFFFFFF;
    struct stat st;
    if (stat(path, &st) == 0) {
        DWORD attr = 0x80; // FILE_ATTRIBUTE_NORMAL
        if (S_ISDIR(st.st_mode)) attr = 0x10; // FILE_ATTRIBUTE_DIRECTORY
        return attr;
    }
    return 0xFFFFFFFF;
}

static uint32_t hle_GetCurrentDirectoryW(struct CPUState* cpu, uint32_t* args) {
    DWORD nBufferLength = args[0];
    DWORD lpBuffer = args[1];

    static const WCHAR cur_dir[] = {'C', ':', '\\', 0};
    DWORD len = sizeof(cur_dir) / sizeof(WCHAR) - 1;

    if (nBufferLength > len && lpBuffer) {
        mem_write(lpBuffer, cur_dir, sizeof(cur_dir));
    }
    return len;
}

static uint32_t hle_GetCurrentDirectoryA(struct CPUState* cpu, uint32_t* args) {
    DWORD nBufferLength = args[0];
    char* lpBuffer = (char*)mem_to_host(args[1]);
    if (!lpBuffer || nBufferLength == 0) return 0;
    if (getcwd(lpBuffer, nBufferLength)) {
        return (uint32_t)strlen(lpBuffer);
    }
    return 0;
}

static uint32_t hle_GetFileInformationByHandle(struct CPUState* cpu, uint32_t* args) {
    void* info = mem_to_host(args[1]);
    if (info) memset(info, 0, 52);
    return 1;
}

static uint32_t hle_GetVolumeInformationA(struct CPUState* cpu, uint32_t* args) {
    if (args[4]) mem_write_u32(args[4], 255);
    if (args[5]) mem_write_u32(args[5], 0x3);
    if (args[6] && args[7] >= 5) {
        char* fs = (char*)mem_to_host(args[6]);
        if (fs) strcpy(fs, "NTFS");
    }
    return 1;
}

static uint32_t hle_CreateMutexA(struct CPUState* cpu, uint32_t* args) {
    return handle_alloc(NULL, HANDLE_TYPE_MUTEX);
}

static uint32_t hle_CreateSemaphoreA(struct CPUState* cpu, uint32_t* args) {
    return handle_alloc(NULL, HANDLE_TYPE_SEMAPHORE);
}

static uint32_t hle_OpenSemaphoreA(struct CPUState* cpu, uint32_t* args) {
    return handle_alloc(NULL, HANDLE_TYPE_SEMAPHORE);
}

static uint32_t hle_SetHandleInformation(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

// FindFirstFile / FindNextFile
#include <dirent.h>

typedef struct {
    DIR* dir;
} WinFind;

static uint32_t hle_FindFirstFileExW(struct CPUState* cpu, uint32_t* args) {
    DWORD fileName_ptr = args[0];
    DWORD fInfoLevelId = args[1];
    DWORD lpFindFileData = args[2];
    DWORD fSearchOp = args[3];
    DWORD lpSearchFilter = args[4];
    DWORD dwAdditionalFlags = args[5];

    DIR* d = opendir(".");
    if (!d) {
        hle_set_last_error(2);
        return (uint32_t)INVALID_HANDLE_VALUE;
    }

    struct dirent* de = readdir(d);
    if (!de) {
        closedir(d);
        hle_set_last_error(18); // ERROR_NO_MORE_FILES
        return (uint32_t)INVALID_HANDLE_VALUE;
    }

    if (lpFindFileData) {
        WIN32_FIND_DATAW fd;
        memset(&fd, 0, sizeof(fd));
        fd.dwFileAttributes = (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) ? 0x10 : 0x80;
        for (int i = 0; de->d_name[i] && i < 259; i++) {
            fd.cFileName[i] = (WCHAR)(uint8_t)de->d_name[i];
        }
        mem_write(lpFindFileData, &fd, sizeof(fd));
    }

    WinFind* wf = (WinFind*)malloc(sizeof(WinFind));
    wf->dir = d;
    return (uint32_t)handle_alloc(wf, HANDLE_TYPE_FIND);
}

static uint32_t hle_FindNextFileW(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFindFile = (HANDLE)args[0];
    DWORD lpFindFileData = args[1];

    WinFind* wf = (WinFind*)handle_get(hFindFile, HANDLE_TYPE_FIND);
    if (!wf || !wf->dir) return 0;

    struct dirent* de = readdir(wf->dir);
    if (!de) {
        hle_set_last_error(18); // ERROR_NO_MORE_FILES
        return 0;
    }

    if (lpFindFileData) {
        WIN32_FIND_DATAW fd;
        memset(&fd, 0, sizeof(fd));
        fd.dwFileAttributes = (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) ? 0x10 : 0x80;
        for (int i = 0; de->d_name[i] && i < 259; i++) {
            fd.cFileName[i] = (WCHAR)(uint8_t)de->d_name[i];
        }
        mem_write(lpFindFileData, &fd, sizeof(fd));
    }
    return 1;
}

static uint32_t hle_FindClose(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFindFile = (HANDLE)args[0];
    WinFind* wf = (WinFind*)handle_get(hFindFile, HANDLE_TYPE_FIND);
    if (wf && wf->dir) {
        closedir(wf->dir);
        free(wf);
    }
    handle_free(hFindFile);
    return 1;
}

static uint32_t hle_CloseHandle(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    void* native_fd = handle_get(h, HANDLE_TYPE_FILE);
    if (native_fd) {
        int fd = (int)(intptr_t)native_fd;
        if (fd > 2) close(fd);
    }
    handle_free(h);
    return 1;
}

// Memory
static uint32_t hle_VirtualAlloc(struct CPUState* cpu, uint32_t* args) {
    return mem_virtual_alloc(args[0], args[1], args[2], args[3]);
}

static uint32_t hle_VirtualFree(struct CPUState* cpu, uint32_t* args) {
    return mem_virtual_free(args[0], args[1], args[2]);
}

static uint32_t hle_VirtualProtect(struct CPUState* cpu, uint32_t* args) {
    DWORD old_prot = 0;
    BOOL res = mem_virtual_protect(args[0], args[1], args[2], &old_prot);
    if (args[3]) mem_write_u32(args[3], old_prot);
    return res;
}

static uint32_t hle_VirtualQuery(struct CPUState* cpu, uint32_t* args) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T res = mem_virtual_query(args[0], &mbi, sizeof(mbi));
    if (res && args[1]) {
        mem_write(args[1], &mbi, sizeof(mbi));
    }
    return res;
}

// Lightweight Heap Manager (for low 512MB RAM target)
static DWORD s_process_heap_base = 0x00400000;

static uint32_t hle_GetProcessHeap(struct CPUState* cpu, uint32_t* args) {
    if (s_process_heap_base == 0x00400000) {
        s_process_heap_base = mem_virtual_alloc(0x00300000, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    return s_process_heap_base;
}

static uint32_t hle_HeapAlloc(struct CPUState* cpu, uint32_t* args) {
    DWORD hHeap = args[0];
    DWORD dwFlags = args[1];
    SIZE_T dwBytes = args[2];

    // Allocate memory from our virtual memory manager (aligned to 16 bytes)
    SIZE_T alloc_size = (dwBytes + 15) & ~15;
    DWORD addr = mem_virtual_alloc(0, alloc_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (addr && (dwFlags & 8)) { // HEAP_ZERO_MEMORY
        void* ptr = mem_to_host(addr);
        if (ptr) memset(ptr, 0, alloc_size);
    }
    return addr;
}

static uint32_t hle_HeapFree(struct CPUState* cpu, uint32_t* args) {
    DWORD hHeap = args[0];
    DWORD dwFlags = args[1];
    DWORD lpMem = args[2];
    if (lpMem) mem_virtual_free(lpMem, 0, MEM_RELEASE);
    return 1;
}

static uint32_t hle_HeapReAlloc(struct CPUState* cpu, uint32_t* args) {
    DWORD hHeap = args[0];
    DWORD dwFlags = args[1];
    DWORD lpMem = args[2];
    SIZE_T dwBytes = args[3];

    DWORD new_addr = mem_virtual_alloc(0, dwBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (new_addr && lpMem) {
        void* src = mem_to_host(lpMem);
        void* dst = mem_to_host(new_addr);
        if (src && dst) memcpy(dst, src, dwBytes);
        mem_virtual_free(lpMem, 0, MEM_RELEASE);
    }
    return new_addr;
}

static uint32_t hle_HeapSize(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x1000;
}

static uint32_t hle_LocalAlloc(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    DWORD uFlags = args[0];
    SIZE_T uBytes = args[1];
    if (uBytes == 0) uBytes = 1;
    SIZE_T alloc_size = (uBytes + 15) & ~15;
    DWORD addr = mem_virtual_alloc(0, alloc_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (addr && (uFlags & 0x0040)) { // LMEM_ZEROINIT
        void* ptr = mem_to_host(addr);
        if (ptr) memset(ptr, 0, alloc_size);
    }
    return addr;
}

static uint32_t hle_LocalFree(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    DWORD hMem = args[0];
    if (hMem) mem_virtual_free(hMem, 0, MEM_RELEASE);
    return 0;
}

static uint32_t hle_LocalLock(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    return args[0];
}

static uint32_t hle_LocalUnlock(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_LocalSize(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x1000;
}

static uint32_t hle_LocalReAlloc(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    DWORD hMem = args[0];
    SIZE_T uBytes = args[1];
    DWORD new_addr = mem_virtual_alloc(0, uBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (new_addr && hMem) {
        void* src = mem_to_host(hMem);
        void* dst = mem_to_host(new_addr);
        if (src && dst) memcpy(dst, src, uBytes);
        mem_virtual_free(hMem, 0, MEM_RELEASE);
    }
    return new_addr;
}

static uint32_t hle_GlobalAlloc(struct CPUState* cpu, uint32_t* args) {
    return hle_LocalAlloc(cpu, args);
}

static uint32_t hle_GlobalFree(struct CPUState* cpu, uint32_t* args) {
    return hle_LocalFree(cpu, args);
}

static uint32_t hle_GlobalLock(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    return args[0];
}

static uint32_t hle_GlobalUnlock(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

// Module & Execution
static uint32_t hle_GetModuleHandleA(struct CPUState* cpu, uint32_t* args) {
    DWORD name_ptr = args[0];
    if (name_ptr == 0) {
        // Return main executable base
        PEB32* peb = (PEB32*)mem_to_host(DEFAULT_PEB_ADDRESS);
        return peb ? peb->ImageBaseAddress : 0x00400000;
    }
    return 0x00400000;
}

static uint32_t hle_GetModuleHandleW(struct CPUState* cpu, uint32_t* args) {
    return hle_GetModuleHandleA(cpu, args);
}

static uint32_t hle_GetProcAddress(struct CPUState* cpu, uint32_t* args) {
    DWORD hModule = args[0];
    DWORD proc_name_ptr = args[1];

    if ((proc_name_ptr >> 16) == 0) {
        // Ordinal import
        WORD ord = (WORD)(proc_name_ptr & 0xFFFF);
        return hle_resolve("kernel32.dll", NULL, ord);
    }

    const char* name = (const char*)mem_to_host(proc_name_ptr);
    uint32_t addr = hle_resolve("kernel32.dll", name, 0);
    if (!addr) addr = hle_resolve("msvcrt.dll", name, 0);
    if (!addr) addr = hle_resolve("user32.dll", name, 0);
    if (!addr) addr = hle_resolve("advapi32.dll", name, 0);
    if (!addr) addr = hle_resolve("ntdll.dll", name, 0);
    if (!addr) addr = hle_register_stub("kernel32.dll", name, 0);
    return addr;
}

static uint32_t hle_LoadLibraryA(struct CPUState* cpu, uint32_t* args) {
    return 0x10000000;
}

static uint32_t hle_LoadLibraryW(struct CPUState* cpu, uint32_t* args) {
    return 0x10000000;
}

static uint32_t hle_LoadLibraryExA(struct CPUState* cpu, uint32_t* args) {
    return 0x10000000;
}

static uint32_t hle_LoadLibraryExW(struct CPUState* cpu, uint32_t* args) {
    return 0x10000000;
}

static uint32_t hle_FreeLibrary(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

static uint32_t hle_GetModuleFileNameA(struct CPUState* cpu, uint32_t* args) {
    DWORD buf_ptr = args[1];
    DWORD buf_len = args[2];
    const char app_name[] = "C:\\app.exe";
    uint32_t len = (uint32_t)strlen(app_name);
    if (len >= buf_len) len = buf_len - 1;
    if (buf_ptr) {
        mem_write(buf_ptr, app_name, len);
        mem_write_u8(buf_ptr + len, 0);
    }
    return len;
}

static uint32_t hle_GetModuleFileNameW(struct CPUState* cpu, uint32_t* args) {
    DWORD buf_ptr = args[1];
    DWORD buf_len = args[2];
    const WCHAR app_name[] = {'C',':','\\','a','p','p','.','e','x','e',0};
    uint32_t len = 10;
    if (len >= buf_len) len = buf_len - 1;
    if (buf_ptr) {
        mem_write(buf_ptr, app_name, len * sizeof(WCHAR));
        mem_write_u16(buf_ptr + len * sizeof(WCHAR), 0);
    }
    return len;
}

static uint32_t hle_SetConsoleCtrlHandler(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

static uint32_t hle_WaitForSingleObject(struct CPUState* cpu, uint32_t* args) {
    return 0; // WAIT_OBJECT_0
}

static uint32_t hle_WaitForMultipleObjects(struct CPUState* cpu, uint32_t* args) {
    return 0; // WAIT_OBJECT_0
}

static uint32_t hle_GetExitCodeProcess(struct CPUState* cpu, uint32_t* args) {
    DWORD code_ptr = args[1];
    if (code_ptr) mem_write_u32(code_ptr, 0);
    return 1;
}

static uint32_t hle_CreateProcessA(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

static uint32_t hle_CreateProcessW(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

static uint32_t hle_GetStartupInfoA(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    if (ptr) {
        // Zero 68 bytes
        uint8_t zeros[68] = {0};
        mem_write(ptr, zeros, sizeof(zeros));
        mem_write_u32(ptr, 68); // cb
    }
    return 0;
}

static uint32_t hle_GetStartupInfoW(struct CPUState* cpu, uint32_t* args) {
    return hle_GetStartupInfoA(cpu, args);
}

static uint32_t hle_GetFileType(struct CPUState* cpu, uint32_t* args) {
    return 2; // FILE_TYPE_CHAR
}

static uint32_t hle_GetConsoleMode(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[1];
    if (ptr) mem_write_u32(ptr, 0x0007);
    return 1;
}

static uint32_t hle_SetConsoleMode(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

static uint32_t hle_GetACP(struct CPUState* cpu, uint32_t* args) {
    return 65001; // UTF-8
}

static uint32_t hle_GetOEMCP(struct CPUState* cpu, uint32_t* args) {
    return 65001;
}

static uint32_t hle_GetCPInfo(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[1];
    if (ptr) {
        mem_write_u32(ptr, 1);       // MaxCharSize
        mem_write_u32(ptr + 4, 0x3F); // DefaultChar
    }
    return 1;
}

static uint32_t hle_MultiByteToWideChar(struct CPUState* cpu, uint32_t* args) {
    DWORD mb_str = args[2];
    int cb_mb = (int)args[3];
    DWORD wc_str = args[4];
    int cch_wc = (int)args[5];

    const char* src = (const char*)mem_to_host(mb_str);
    if (!src) return 0;
    if (cb_mb < 0) cb_mb = (int)strlen(src) + 1;
    if (cch_wc == 0) return (uint32_t)cb_mb;

    WCHAR* dst = (WCHAR*)mem_to_host(wc_str);
    if (!dst) return 0;
    int count = (cb_mb < cch_wc) ? cb_mb : cch_wc;
    for (int i = 0; i < count; i++) {
        dst[i] = (WCHAR)(unsigned char)src[i];
    }
    return (uint32_t)count;
}

static uint32_t hle_WideCharToMultiByte(struct CPUState* cpu, uint32_t* args) {
    DWORD wc_str = args[2];
    int cch_wc = (int)args[3];
    DWORD mb_str = args[4];
    int cb_mb = (int)args[5];

    const WCHAR* src = (const WCHAR*)mem_to_host(wc_str);
    if (!src) return 0;
    if (cch_wc < 0) {
        cch_wc = 0;
        while (src[cch_wc] != 0) cch_wc++;
        cch_wc++;
    }
    if (cb_mb == 0) return (uint32_t)cch_wc;

    char* dst = (char*)mem_to_host(mb_str);
    if (!dst) return 0;
    int count = (cch_wc < cb_mb) ? cch_wc : cb_mb;
    for (int i = 0; i < count; i++) {
        dst[i] = (char)(src[i] & 0xFF);
    }
    return (uint32_t)count;
}

static uint32_t hle_FormatMessageA(struct CPUState* cpu, uint32_t* args) {
    DWORD buf = args[5];
    DWORD size = args[6];
    if (buf && size > 16) {
        mem_write(buf, "Success.\0", 9);
        return 8;
    }
    return 0;
}

static uint32_t hle_FormatMessageW(struct CPUState* cpu, uint32_t* args) {
    DWORD buf = args[5];
    DWORD size = args[6];
    if (buf && size > 16) {
        const WCHAR succ[] = {'O','K',0};
        mem_write(buf, succ, sizeof(succ));
        return 2;
    }
    return 0;
}

static uint32_t hle_SearchPathA(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_GetTempPathA(struct CPUState* cpu, uint32_t* args) {
    DWORD buf_len = args[0];
    DWORD buf = args[1];
    const char tmp[] = "/tmp/\0";
    if (buf && buf_len > sizeof(tmp)) {
        mem_write(buf, tmp, sizeof(tmp));
        return (uint32_t)(sizeof(tmp) - 1);
    }
    return 0;
}

static uint32_t hle_TerminateProcess(struct CPUState* cpu, uint32_t* args) {
    extern void cpu_halt(struct CPUState* cpu, uint32_t exit_code);
    cpu_halt(cpu, args[1]);
    return 1;
}

static uint32_t hle_DuplicateHandle(struct CPUState* cpu, uint32_t* args) {
    DWORD target_ptr = args[4];
    if (target_ptr) mem_write_u32(target_ptr, args[1]);
    return 1;
}

static uint32_t hle_CreatePipe(struct CPUState* cpu, uint32_t* args) {
    DWORD read_ptr = args[0];
    DWORD write_ptr = args[1];
    if (read_ptr) mem_write_u32(read_ptr, 100);
    if (write_ptr) mem_write_u32(write_ptr, 101);
    return 1;
}

static uint32_t hle_wsprintfA(struct CPUState* cpu, uint32_t* args) {
    char* buf = (char*)mem_to_host(args[0]);
    const char* fmt = (const char*)mem_to_host(args[1]);
    if (!buf || !fmt) return 0;
    // Format simple string
    strcpy(buf, fmt);
    return (uint32_t)strlen(buf);
}

static uint32_t hle_InitializeSecurityDescriptor(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

// TLS (Thread Local Storage)
static uint32_t s_tls_slots[64] = {0};
static uint32_t s_tls_next_index = 0;

static uint32_t hle_TlsAlloc(struct CPUState* cpu, uint32_t* args) {
    if (s_tls_next_index < 64) {
        return s_tls_next_index++;
    }
    return 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
}

static uint32_t hle_TlsGetValue(struct CPUState* cpu, uint32_t* args) {
    DWORD idx = args[0];
    if (idx < 64) return s_tls_slots[idx];
    return 0;
}

static uint32_t hle_TlsSetValue(struct CPUState* cpu, uint32_t* args) {
    DWORD idx = args[0];
    DWORD val = args[1];
    if (idx < 64) {
        s_tls_slots[idx] = val;
        return 1;
    }
    return 0;
}

static uint32_t hle_TlsFree(struct CPUState* cpu, uint32_t* args) {
    return 1;
}

// Error Handling
static uint32_t hle_GetLastError(struct CPUState* cpu, uint32_t* args) {
    return hle_get_last_error();
}

static uint32_t hle_SetLastError(struct CPUState* cpu, uint32_t* args) {
    hle_set_last_error(args[0]);
    return 0;
}

// Timing
static uint32_t hle_GetTickCount(struct CPUState* cpu, uint32_t* args) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (DWORD)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}

static uint32_t hle_GetSystemTimeAsFileTime(struct CPUState* cpu, uint32_t* args) {
    DWORD out_ptr = args[0];
    if (!out_ptr) return 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t win_time = ((uint64_t)ts.tv_sec + 11644473600ULL) * 10000000ULL + ((uint64_t)ts.tv_nsec / 100ULL);

    mem_write_u32(out_ptr, (DWORD)(win_time & 0xFFFFFFFF));
    mem_write_u32(out_ptr + 4, (DWORD)(win_time >> 32));
    return 0;
}

static uint32_t hle_QueryPerformanceCounter(struct CPUState* cpu, uint32_t* args) {
    DWORD out_ptr = args[0];
    if (!out_ptr) return 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t val = ((uint64_t)ts.tv_sec * 10000000ULL) + ((uint64_t)ts.tv_nsec / 100ULL);

    mem_write_u32(out_ptr, (DWORD)(val & 0xFFFFFFFF));
    mem_write_u32(out_ptr + 4, (DWORD)(val >> 32));
    return 1;
}

static uint32_t hle_QueryPerformanceFrequency(struct CPUState* cpu, uint32_t* args) {
    DWORD out_ptr = args[0];
    if (!out_ptr) return 0;
    // 10 MHz timer
    mem_write_u32(out_ptr, 10000000);
    mem_write_u32(out_ptr + 4, 0);
    return 1;
}

static uint32_t hle_Sleep(struct CPUState* cpu, uint32_t* args) {
    DWORD ms = args[0];
    usleep(ms * 1000);
    return 0;
}

// Critical Section (Classic Win32)
static uint32_t hle_InitializeCriticalSection(struct CPUState* cpu, uint32_t* args) {
    DWORD cs_addr = args[0];
    if (cs_addr) {
        pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(m, NULL);
        mem_write_u32(cs_addr + 12, (DWORD)(uintptr_t)m); // Store pointer at OwningThread or LockData
    }
    return 0;
}

static uint32_t hle_InitializeCriticalSectionAndSpinCount(struct CPUState* cpu, uint32_t* args) {
    return hle_InitializeCriticalSection(cpu, args);
}

static uint32_t hle_EnterCriticalSection(struct CPUState* cpu, uint32_t* args) {
    DWORD cs_addr = args[0];
    if (cs_addr) {
        pthread_mutex_t* m = (pthread_mutex_t*)(uintptr_t)mem_read_u32(cs_addr + 12);
        if (m) pthread_mutex_lock(m);
    }
    return 0;
}

static uint32_t hle_LeaveCriticalSection(struct CPUState* cpu, uint32_t* args) {
    DWORD cs_addr = args[0];
    if (cs_addr) {
        pthread_mutex_t* m = (pthread_mutex_t*)(uintptr_t)mem_read_u32(cs_addr + 12);
        if (m) pthread_mutex_unlock(m);
    }
    return 0;
}

static uint32_t hle_DeleteCriticalSection(struct CPUState* cpu, uint32_t* args) {
    DWORD cs_addr = args[0];
    if (cs_addr) {
        pthread_mutex_t* m = (pthread_mutex_t*)(uintptr_t)mem_read_u32(cs_addr + 12);
        if (m) {
            pthread_mutex_destroy(m);
            free(m);
            mem_write_u32(cs_addr + 12, 0);
        }
    }
    return 0;
}

// Version APIs
static uint32_t hle_GetVersionExA(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    PEB32* peb = (PEB32*)mem_to_host(DEFAULT_PEB_ADDRESS);
    if (!ptr || !peb) return 0;

    mem_write_u32(ptr + 4, peb->OSMajorVersion);
    mem_write_u32(ptr + 8, peb->OSMinorVersion);
    mem_write_u32(ptr + 12, peb->OSBuildNumber);
    mem_write_u32(ptr + 16, peb->OSPlatformId);
    return 1;
}

static uint32_t hle_GetVersionExW(struct CPUState* cpu, uint32_t* args) {
    return hle_GetVersionExA(cpu, args);
}

static uint32_t hle_GetSystemInfo(struct CPUState* cpu, uint32_t* args) {
    DWORD ptr = args[0];
    if (!ptr) return 0;

    SYSTEM_INFO info;
    memset(&info, 0, sizeof(info));
    info.wProcessorArchitecture = 0; // PROCESSOR_ARCHITECTURE_INTEL
    info.dwPageSize = PAGE_SIZE_4K;
    info.lpMinimumApplicationAddress = 0x00010000;
    info.lpMaximumApplicationAddress = 0x7FFEFFFF;
    info.dwActiveProcessorMask = 1;
    info.dwNumberOfProcessors = 2;
    info.dwAllocationGranularity = ALLOCATION_GRANULARITY;
    info.wProcessorLevel = 6;

    mem_write(ptr, &info, sizeof(info));
    return 0;
}

static DWORD s_cmdline_addr = 0;
static DWORD s_cmdlineW_addr = 0;

static uint32_t hle_GetCommandLineA(struct CPUState* cpu, uint32_t* args) {
    if (!s_cmdline_addr) {
        s_cmdline_addr = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        mem_write(s_cmdline_addr, "app.exe\0", 8);
    }
    return s_cmdline_addr;
}

static uint32_t hle_GetCommandLineW(struct CPUState* cpu, uint32_t* args) {
    if (!s_cmdlineW_addr) {
        s_cmdlineW_addr = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const WCHAR w[] = {'a','p','p','.','e','x','e',0};
        mem_write(s_cmdlineW_addr, w, sizeof(w));
    }
    return s_cmdlineW_addr;
}

void kernel32_set_command_line(int argc, char** argv) {
    if (!s_cmdline_addr) {
        s_cmdline_addr = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!s_cmdlineW_addr) {
        s_cmdlineW_addr = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    char buf[1024] = {0};
    size_t pos = 0;
    for (int i = 0; i < argc && pos < sizeof(buf) - 2; i++) {
        if (i > 0) buf[pos++] = ' ';
        size_t len = strlen(argv[i]);
        if (pos + len >= sizeof(buf) - 2) break;
        memcpy(buf + pos, argv[i], len);
        pos += len;
    }
    if (pos == 0) {
        strcpy(buf, "app.exe");
        pos = 7;
    }
    buf[pos] = '\0';
    mem_write(s_cmdline_addr, buf, pos + 1);

    WCHAR bufW[1024] = {0};
    for (size_t i = 0; i < pos; i++) bufW[i] = (WCHAR)(unsigned char)buf[i];
    bufW[pos] = 0;
    mem_write(s_cmdlineW_addr, bufW, (pos + 1) * sizeof(WCHAR));
}

void register_kernel32_base_apis(void) {
    hle_register("kernel32.dll", "ExitProcess", 0, hle_ExitProcess, 1, false);
    hle_register("kernel32.dll", "GetCurrentProcess", 0, hle_GetCurrentProcess, 0, false);
    hle_register("kernel32.dll", "GetCurrentThread", 0, hle_GetCurrentThread, 0, false);
    hle_register("kernel32.dll", "GetCurrentProcessId", 0, hle_GetCurrentProcessId, 0, false);
    hle_register("kernel32.dll", "GetCurrentThreadId", 0, hle_GetCurrentThreadId, 0, false);

    hle_register("kernel32.dll", "GetStdHandle", 0, hle_GetStdHandle, 1, false);
    hle_register("kernel32.dll", "SetStdHandle", 0, hle_SetStdHandle, 2, false);
    hle_register("kernel32.dll", "WriteFile", 0, hle_WriteFile, 5, false);
    hle_register("kernel32.dll", "ReadFile", 0, hle_ReadFile, 5, false);
    hle_register("kernel32.dll", "CreateFileA", 0, hle_CreateFileA, 7, false);
    hle_register("kernel32.dll", "CreateFileW", 0, hle_CreateFileW, 7, false);
    hle_register("kernel32.dll", "DeleteFileW", 0, hle_DeleteFileW, 1, false);
    hle_register("kernel32.dll", "GetFileAttributesW", 0, hle_GetFileAttributesW, 1, false);
    hle_register("kernel32.dll", "GetFileAttributesA", 0, hle_GetFileAttributesA, 1, false);
    hle_register("kernel32.dll", "GetCurrentDirectoryW", 0, hle_GetCurrentDirectoryW, 2, false);
    hle_register("kernel32.dll", "GetCurrentDirectoryA", 0, hle_GetCurrentDirectoryA, 2, false);
    hle_register("kernel32.dll", "GetFileInformationByHandle", 0, hle_GetFileInformationByHandle, 2, false);
    hle_register("kernel32.dll", "GetVolumeInformationA", 0, hle_GetVolumeInformationA, 8, false);
    hle_register("kernel32.dll", "CreateMutexA", 0, hle_CreateMutexA, 3, false);
    hle_register("kernel32.dll", "CreateSemaphoreA", 0, hle_CreateSemaphoreA, 4, false);
    hle_register("kernel32.dll", "OpenSemaphoreA", 0, hle_OpenSemaphoreA, 3, false);
    hle_register("kernel32.dll", "SetHandleInformation", 0, hle_SetHandleInformation, 3, false);
    hle_register("kernel32.dll", "FindFirstFileExW", 0, hle_FindFirstFileExW, 6, false);
    hle_register("kernel32.dll", "FindNextFileW", 0, hle_FindNextFileW, 2, false);
    hle_register("kernel32.dll", "FindClose", 0, hle_FindClose, 1, false);
    hle_register("kernel32.dll", "WriteConsoleA", 0, hle_WriteConsoleA, 5, false);
    hle_register("kernel32.dll", "WriteConsoleW", 0, hle_WriteConsoleW, 5, false);
    hle_register("kernel32.dll", "CloseHandle", 0, hle_CloseHandle, 1, false);

    hle_register("kernel32.dll", "VirtualAlloc", 0, hle_VirtualAlloc, 4, false);
    hle_register("kernel32.dll", "VirtualFree", 0, hle_VirtualFree, 3, false);
    hle_register("kernel32.dll", "VirtualProtect", 0, hle_VirtualProtect, 4, false);
    hle_register("kernel32.dll", "VirtualQuery", 0, hle_VirtualQuery, 3, false);

    hle_register("kernel32.dll", "GetProcessHeap", 0, hle_GetProcessHeap, 0, false);
    hle_register("kernel32.dll", "HeapAlloc", 0, hle_HeapAlloc, 3, false);
    hle_register("kernel32.dll", "HeapFree", 0, hle_HeapFree, 3, false);
    hle_register("kernel32.dll", "HeapReAlloc", 0, hle_HeapReAlloc, 4, false);
    hle_register("kernel32.dll", "HeapSize", 0, hle_HeapSize, 3, false);

    hle_register("kernel32.dll", "GetModuleHandleA", 0, hle_GetModuleHandleA, 1, false);
    hle_register("kernel32.dll", "GetModuleHandleW", 0, hle_GetModuleHandleW, 1, false);
    hle_register("kernel32.dll", "GetProcAddress", 0, hle_GetProcAddress, 2, false);

    hle_register("kernel32.dll", "TlsAlloc", 0, hle_TlsAlloc, 0, false);
    hle_register("kernel32.dll", "TlsGetValue", 0, hle_TlsGetValue, 1, false);
    hle_register("kernel32.dll", "TlsSetValue", 0, hle_TlsSetValue, 2, false);
    hle_register("kernel32.dll", "TlsFree", 0, hle_TlsFree, 1, false);

    hle_register("kernel32.dll", "GetLastError", 0, hle_GetLastError, 0, false);
    hle_register("kernel32.dll", "SetLastError", 0, hle_SetLastError, 1, false);

    hle_register("kernel32.dll", "GetTickCount", 0, hle_GetTickCount, 0, false);
    hle_register("kernel32.dll", "GetSystemTimeAsFileTime", 0, hle_GetSystemTimeAsFileTime, 1, false);
    hle_register("kernel32.dll", "QueryPerformanceCounter", 0, hle_QueryPerformanceCounter, 1, false);
    hle_register("kernel32.dll", "QueryPerformanceFrequency", 0, hle_QueryPerformanceFrequency, 1, false);
    hle_register("kernel32.dll", "Sleep", 0, hle_Sleep, 1, false);

    hle_register("kernel32.dll", "InitializeCriticalSection", 0, hle_InitializeCriticalSection, 1, false);
    hle_register("kernel32.dll", "InitializeCriticalSectionAndSpinCount", 0, hle_InitializeCriticalSectionAndSpinCount, 2, false);
    hle_register("kernel32.dll", "EnterCriticalSection", 0, hle_EnterCriticalSection, 1, false);
    hle_register("kernel32.dll", "LeaveCriticalSection", 0, hle_LeaveCriticalSection, 1, false);
    hle_register("kernel32.dll", "DeleteCriticalSection", 0, hle_DeleteCriticalSection, 1, false);

    hle_register("kernel32.dll", "GetVersionExA", 0, hle_GetVersionExA, 1, false);
    hle_register("kernel32.dll", "GetVersionExW", 0, hle_GetVersionExW, 1, false);
    hle_register("kernel32.dll", "GetSystemInfo", 0, hle_GetSystemInfo, 1, false);
    hle_register("kernel32.dll", "GetNativeSystemInfo", 0, hle_GetSystemInfo, 1, false);

    hle_register("kernel32.dll", "GetCommandLineA", 0, hle_GetCommandLineA, 0, false);
    hle_register("kernel32.dll", "GetCommandLineW", 0, hle_GetCommandLineW, 0, false);

    hle_register("kernel32.dll", "LoadLibraryA", 0, hle_LoadLibraryA, 1, false);
    hle_register("kernel32.dll", "LoadLibraryW", 0, hle_LoadLibraryW, 1, false);
    hle_register("kernel32.dll", "LoadLibraryExA", 0, hle_LoadLibraryExA, 3, false);
    hle_register("kernel32.dll", "LoadLibraryExW", 0, hle_LoadLibraryExW, 3, false);
    hle_register("kernel32.dll", "FreeLibrary", 0, hle_FreeLibrary, 1, false);

    hle_register("kernel32.dll", "GetModuleFileNameA", 0, hle_GetModuleFileNameA, 3, false);
    hle_register("kernel32.dll", "GetModuleFileNameW", 0, hle_GetModuleFileNameW, 3, false);
    hle_register("kernel32.dll", "SetConsoleCtrlHandler", 0, hle_SetConsoleCtrlHandler, 2, false);
    hle_register("kernel32.dll", "WaitForSingleObject", 0, hle_WaitForSingleObject, 2, false);
    hle_register("kernel32.dll", "WaitForMultipleObjects", 0, hle_WaitForMultipleObjects, 4, false);
    hle_register("kernel32.dll", "GetExitCodeProcess", 0, hle_GetExitCodeProcess, 2, false);
    hle_register("kernel32.dll", "CreateProcessA", 0, hle_CreateProcessA, 10, false);
    hle_register("kernel32.dll", "CreateProcessW", 0, hle_CreateProcessW, 10, false);
    hle_register("kernel32.dll", "TerminateProcess", 0, hle_TerminateProcess, 2, false);

    hle_register("kernel32.dll", "GetStartupInfoA", 0, hle_GetStartupInfoA, 1, false);
    hle_register("kernel32.dll", "GetStartupInfoW", 0, hle_GetStartupInfoW, 1, false);
    hle_register("kernel32.dll", "GetFileType", 0, hle_GetFileType, 1, false);
    hle_register("kernel32.dll", "GetConsoleMode", 0, hle_GetConsoleMode, 2, false);
    hle_register("kernel32.dll", "SetConsoleMode", 0, hle_SetConsoleMode, 2, false);
    hle_register("kernel32.dll", "GetACP", 0, hle_GetACP, 0, false);
    hle_register("kernel32.dll", "GetOEMCP", 0, hle_GetOEMCP, 0, false);
    hle_register("kernel32.dll", "GetCPInfo", 0, hle_GetCPInfo, 2, false);

    hle_register("kernel32.dll", "MultiByteToWideChar", 0, hle_MultiByteToWideChar, 6, false);
    hle_register("kernel32.dll", "WideCharToMultiByte", 0, hle_WideCharToMultiByte, 8, false);
    hle_register("kernel32.dll", "FormatMessageA", 0, hle_FormatMessageA, 7, false);
    hle_register("kernel32.dll", "FormatMessageW", 0, hle_FormatMessageW, 7, false);
    hle_register("kernel32.dll", "SearchPathA", 0, hle_SearchPathA, 6, false);
    hle_register("kernel32.dll", "GetTempPathA", 0, hle_GetTempPathA, 2, false);
    hle_register("kernel32.dll", "DuplicateHandle", 0, hle_DuplicateHandle, 7, false);
    hle_register("kernel32.dll", "CreatePipe", 0, hle_CreatePipe, 4, false);

    hle_register("user32.dll", "wsprintfA", 0, hle_wsprintfA, 2, true);
    hle_register("advapi32.dll", "InitializeSecurityDescriptor", 0, hle_InitializeSecurityDescriptor, 2, false);

    // Local & Global Heap Allocators
    hle_register("kernel32.dll", "LocalAlloc", 0, hle_LocalAlloc, 2, false);
    hle_register("kernel32.dll", "LocalFree", 0, hle_LocalFree, 1, false);
    hle_register("kernel32.dll", "LocalLock", 0, hle_LocalLock, 1, false);
    hle_register("kernel32.dll", "LocalUnlock", 0, hle_LocalUnlock, 1, false);
    hle_register("kernel32.dll", "LocalSize", 0, hle_LocalSize, 1, false);
    hle_register("kernel32.dll", "LocalReAlloc", 0, hle_LocalReAlloc, 3, false);
    hle_register("kernel32.dll", "GlobalAlloc", 0, hle_GlobalAlloc, 2, false);
    hle_register("kernel32.dll", "GlobalFree", 0, hle_GlobalFree, 1, false);
    hle_register("kernel32.dll", "GlobalLock", 0, hle_GlobalLock, 1, false);
    hle_register("kernel32.dll", "GlobalUnlock", 0, hle_GlobalUnlock, 1, false);
}
