#include "hle.h"
#include "sync.h"
#include "memory.h"
#include "teb_peb.h"
#include "cpu.h"
#include "pe.h"
#include "apiset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>

// -------------------------------------------------------------
// Windows 7 / 10 Synchronization APIs
// -------------------------------------------------------------

static uint32_t hle_InitializeSRWLock(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    win7_init_srwlock(lock);
    return 0;
}

static uint32_t hle_AcquireSRWLockExclusive(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    win7_acquire_srwlock_exclusive(lock);
    return 0;
}

static uint32_t hle_ReleaseSRWLockExclusive(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    win7_release_srwlock_exclusive(lock);
    return 0;
}

static uint32_t hle_AcquireSRWLockShared(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    win7_acquire_srwlock_shared(lock);
    return 0;
}

static uint32_t hle_ReleaseSRWLockShared(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    win7_release_srwlock_shared(lock);
    return 0;
}

static uint32_t hle_TryAcquireSRWLockExclusive(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    return win7_try_acquire_srwlock_exclusive(lock);
}

static uint32_t hle_TryAcquireSRWLockShared(struct CPUState* cpu, uint32_t* args) {
    DWORD lock_addr = args[0];
    PSRWLOCK lock = (PSRWLOCK)mem_to_host(lock_addr);
    return win7_try_acquire_srwlock_shared(lock);
}

static uint32_t hle_InitializeConditionVariable(struct CPUState* cpu, uint32_t* args) {
    DWORD cv_addr = args[0];
    PCONDITION_VARIABLE cv = (PCONDITION_VARIABLE)mem_to_host(cv_addr);
    win7_init_condition_variable(cv);
    return 0;
}

static uint32_t hle_WakeConditionVariable(struct CPUState* cpu, uint32_t* args) {
    DWORD cv_addr = args[0];
    PCONDITION_VARIABLE cv = (PCONDITION_VARIABLE)mem_to_host(cv_addr);
    win7_wake_condition_variable(cv);
    return 0;
}

static uint32_t hle_WakeAllConditionVariable(struct CPUState* cpu, uint32_t* args) {
    DWORD cv_addr = args[0];
    PCONDITION_VARIABLE cv = (PCONDITION_VARIABLE)mem_to_host(cv_addr);
    win7_wake_all_condition_variable(cv);
    return 0;
}

static uint32_t hle_SleepConditionVariableSRW(struct CPUState* cpu, uint32_t* args) {
    DWORD cv_addr = args[0];
    DWORD srw_addr = args[1];
    DWORD ms = args[2];
    ULONG flags = args[3];

    PCONDITION_VARIABLE cv = (PCONDITION_VARIABLE)mem_to_host(cv_addr);
    PSRWLOCK srw = (PSRWLOCK)mem_to_host(srw_addr);
    return win7_sleep_condition_variable_srw(cv, srw, ms, flags);
}

static uint32_t hle_InitOnceInitialize(struct CPUState* cpu, uint32_t* args) {
    DWORD init_once_addr = args[0];
    PINIT_ONCE io = (PINIT_ONCE)mem_to_host(init_once_addr);
    win7_init_once_initialize(io);
    return 0;
}

static uint32_t hle_InitOnceBeginInitialize(struct CPUState* cpu, uint32_t* args) {
    DWORD io_addr = args[0];
    DWORD flags = args[1];
    DWORD pending_ptr = args[2];
    DWORD ctx_ptr = args[3];

    PINIT_ONCE io = (PINIT_ONCE)mem_to_host(io_addr);
    BOOL pending = FALSE;
    LPVOID ctx = 0;

    BOOL res = win7_init_once_begin(io, flags, &pending, &ctx);
    if (pending_ptr) mem_write_u32(pending_ptr, pending);
    if (ctx_ptr) mem_write_u32(ctx_ptr, (DWORD)(uintptr_t)ctx);
    return res;
}

static uint32_t hle_InitOnceComplete(struct CPUState* cpu, uint32_t* args) {
    DWORD io_addr = args[0];
    DWORD flags = args[1];
    LPVOID ctx = (LPVOID)(uintptr_t)args[2];

    PINIT_ONCE io = (PINIT_ONCE)mem_to_host(io_addr);
    return win7_init_once_complete(io, flags, ctx);
}

// Windows 8/10 WaitOnAddress (Futex)
static uint32_t hle_WaitOnAddress(struct CPUState* cpu, uint32_t* args) {
    DWORD addr = args[0];
    DWORD cmp_addr = args[1];
    SIZE_T size = args[2];
    DWORD ms = args[3];
    return win10_wait_on_address(addr, cmp_addr, size, ms);
}

static uint32_t hle_WakeByAddressSingle(struct CPUState* cpu, uint32_t* args) {
    DWORD addr = args[0];
    win10_wake_by_address_single(addr);
    return 0;
}

static uint32_t hle_WakeByAddressAll(struct CPUState* cpu, uint32_t* args) {
    DWORD addr = args[0];
    win10_wake_by_address_all(addr);
    return 0;
}

// -------------------------------------------------------------
// High-Precision Time APIs (Windows 8 / 10)
// -------------------------------------------------------------

static uint32_t hle_GetSystemTimePreciseAsFileTime(struct CPUState* cpu, uint32_t* args) {
    DWORD out_ptr = args[0];
    if (!out_ptr) return 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // 100ns units since Jan 1 1601
    uint64_t win_time = ((uint64_t)ts.tv_sec + 11644473600ULL) * 10000000ULL + ((uint64_t)ts.tv_nsec / 100ULL);

    mem_write_u32(out_ptr, (DWORD)(win_time & 0xFFFFFFFF));
    mem_write_u32(out_ptr + 4, (DWORD)(win_time >> 32));
    return 0;
}

static uint32_t hle_GetTickCount64(struct CPUState* cpu, uint32_t* args) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    // In x86 stdcall, 64-bit return values are in EDX:EAX
    // Return low 32 bits from handler; caller will store high 32 bits into EDX
    extern void cpu_set_edx(struct CPUState* cpu, uint32_t val);
    cpu_set_edx(cpu, (uint32_t)(ms >> 32));
    return (uint32_t)(ms & 0xFFFFFFFF);
}

// -------------------------------------------------------------
// Extended System & Processor Information (Windows 7 / 10)
// -------------------------------------------------------------

static uint32_t hle_GetLogicalProcessorInformationEx(struct CPUState* cpu, uint32_t* args) {
    DWORD relationship = args[0];
    DWORD buffer_ptr = args[1];
    DWORD length_ptr = args[2];

    if (!length_ptr) return 0;

    // Report 2 cores / 1 package
    DWORD needed = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    DWORD current_len = mem_read_u32(length_ptr);

    if (current_len < needed || !buffer_ptr) {
        mem_write_u32(length_ptr, needed);
        hle_set_last_error(122); // ERROR_INSUFFICIENT_BUFFER
        return 0;
    }

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info;
    memset(&info, 0, sizeof(info));
    info.Relationship = RelationProcessorCore;
    info.Size = sizeof(info);
    info.u.Processor.Flags = 0;
    info.u.Processor.GroupCount = 1;

    mem_write(buffer_ptr, &info, sizeof(info));
    mem_write_u32(length_ptr, needed);
    return 1;
}

static uint32_t hle_GetProductInfo(struct CPUState* cpu, uint32_t* args) {
    DWORD os_major = args[0];
    DWORD os_minor = args[1];
    DWORD sp_major = args[2];
    DWORD sp_minor = args[3];
    DWORD product_type_ptr = args[4];

    if (product_type_ptr) {
        // 0x00000030 = PRODUCT_PROFESSIONAL
        mem_write_u32(product_type_ptr, 0x00000030);
    }
    return 1;
}

// -------------------------------------------------------------
// Modern Memory & NUMA (512MB RAM Budget Awareness)
// -------------------------------------------------------------

static uint32_t hle_VirtualAllocExNuma(struct CPUState* cpu, uint32_t* args) {
    // args: hProcess, lpAddress, dwSize, flAllocationType, flProtect, nndPreferred
    DWORD addr = args[1];
    SIZE_T size = args[2];
    DWORD alloc_type = args[3];
    DWORD protect = args[4];
    // nndPreferred is ignored in single-node embedded system
    return mem_virtual_alloc(addr, size, alloc_type, protect);
}

static uint32_t hle_GetLargePageMinimum(struct CPUState* cpu, uint32_t* args) {
    // 0 = large pages not supported / disabled (avoids wasting RAM on 512MB hardware!)
    return 0;
}

static uint32_t hle_PrefetchVirtualMemory(struct CPUState* cpu, uint32_t* args) {
    // args: hProcess, NumberOfEntries, VirtualAddresses, Flags
    // No-op hint on low-RAM embedded hardware
    return 1;
}

static uint32_t hle_DiscardVirtualMemory(struct CPUState* cpu, uint32_t* args) {
    // args: VirtualAddress, Size
    // Tell OS kernel memory is not needed (MADV_DONTNEED)
    DWORD addr = args[0];
    SIZE_T size = args[1];
    return mem_virtual_free(addr, size, MEM_DECOMMIT);
}

// -------------------------------------------------------------
// Modern String & Locale (NLS) APIs (Windows 7+)
// -------------------------------------------------------------

static uint32_t hle_GetUserDefaultLocaleName(struct CPUState* cpu, uint32_t* args) {
    DWORD buf_ptr = args[0];
    int cch = (int)args[1];

    static const WCHAR locale_name[] = {'e', 'n', '-', 'U', 'S', 0};
    int len = sizeof(locale_name) / sizeof(WCHAR);

    if (cch > 0 && buf_ptr) {
        int copy_len = (cch < len) ? cch : len;
        mem_write(buf_ptr, locale_name, copy_len * sizeof(WCHAR));
    }
    return len;
}

static uint32_t hle_ResolveLocaleName(struct CPUState* cpu, uint32_t* args) {
    DWORD src_ptr = args[0];
    DWORD dst_ptr = args[1];
    int cch = (int)args[2];

    static const WCHAR locale_name[] = {'e', 'n', '-', 'U', 'S', 0};
    int len = sizeof(locale_name) / sizeof(WCHAR);

    if (cch > 0 && dst_ptr) {
        int copy_len = (cch < len) ? cch : len;
        mem_write(dst_ptr, locale_name, copy_len * sizeof(WCHAR));
    }
    return len;
}

static uint32_t hle_GetLocaleInfoEx(struct CPUState* cpu, uint32_t* args) {
    DWORD locale_ptr = args[0];
    DWORD lc_type = args[1];
    DWORD data_ptr = args[2];
    int cch_data = (int)args[3];

    static const WCHAR def_val[] = {'0', 0};
    if (cch_data > 0 && data_ptr) {
        mem_write(data_ptr, def_val, sizeof(def_val));
    }
    return 2;
}

static uint32_t hle_IdnToAscii(struct CPUState* cpu, uint32_t* args) {
    DWORD flags = args[0];
    DWORD src_ptr = args[1];
    int src_len = (int)args[2];
    DWORD dst_ptr = args[3];
    int dst_len = (int)args[4];

    if (src_len < 0) {
        // Null terminated WCHAR string
        src_len = 0;
        while (mem_read_u16(src_ptr + src_len * 2) != 0) src_len++;
        src_len++; // include null
    }

    if (dst_len > 0 && dst_ptr) {
        int to_copy = (dst_len < src_len) ? dst_len : src_len;
        for (int i = 0; i < to_copy; i++) {
            uint16_t ch = mem_read_u16(src_ptr + i * 2);
            mem_write_u16(dst_ptr + i * 2, ch);
        }
    }
    return src_len;
}

// -------------------------------------------------------------
// Threadpool APIs (Vista / Windows 7+)
// -------------------------------------------------------------

static uint32_t hle_CreateThreadpoolWork(struct CPUState* cpu, uint32_t* args) {
    // args: pfnwk, pv, pcbe
    DWORD pfn = args[0];
    DWORD context = args[1];
    // Return a dummy work handle
    return (uint32_t)handle_alloc((void*)(uintptr_t)pfn, HANDLE_TYPE_THREAD);
}

static uint32_t hle_SubmitThreadpoolWork(struct CPUState* cpu, uint32_t* args) {
    // Synchronous execution or queued
    return 0;
}

static uint32_t hle_CloseThreadpoolWork(struct CPUState* cpu, uint32_t* args) {
    handle_free((HANDLE)args[0]);
    return 0;
}

static uint32_t hle_WaitForThreadpoolWorkCallbacks(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

// -------------------------------------------------------------
// Accurate 512MB RAM Reporting (GlobalMemoryStatusEx)
// -------------------------------------------------------------

static uint32_t hle_GlobalMemoryStatusEx(struct CPUState* cpu, uint32_t* args) {
    DWORD status_ptr = args[0];
    if (!status_ptr) return 0;

    MEMORYSTATUSEX status;
    memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(MEMORYSTATUSEX);

    // 512 MB physical hardware budget
    ULONGLONG total_phys = 512ULL * 1024 * 1024;
    ULONGLONG committed = (ULONGLONG)g_mem.current_committed;
    ULONGLONG avail_phys = (total_phys > committed) ? (total_phys - committed) : (16ULL * 1024 * 1024);

    status.ullTotalPhys = total_phys;
    status.ullAvailPhys = avail_phys;
    status.ullTotalPageFile = 1024ULL * 1024 * 1024; // 1 GB simulated pagefile
    status.ullAvailPageFile = 1024ULL * 1024 * 1024 - committed;
    status.ullTotalVirtual = WIN32_VIRTUAL_SPACE_SIZE;
    status.ullAvailVirtual = (WIN32_VIRTUAL_SPACE_SIZE > committed) ? (WIN32_VIRTUAL_SPACE_SIZE - committed) : 0;
    status.dwMemoryLoad = (DWORD)((committed * 100ULL) / total_phys);
    if (status.dwMemoryLoad > 100) status.dwMemoryLoad = 100;

    mem_write(status_ptr, &status, sizeof(status));
    return 1;
}

// -------------------------------------------------------------
// Extended File APIs (Windows Vista / 7 / 8 / 10)
// -------------------------------------------------------------

static uint32_t hle_GetFileInformationByHandleEx(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFile = (HANDLE)args[0];
    FILE_INFO_BY_HANDLE_CLASS info_class = (FILE_INFO_BY_HANDLE_CLASS)args[1];
    DWORD lpFileInformation = args[2];
    DWORD dwBufferSize = args[3];

    void* native_fd = handle_get(hFile, HANDLE_TYPE_FILE);
    int fd = (int)(intptr_t)native_fd;
    if (hFile == (HANDLE)(uint32_t)-10) fd = 0;
    else if (hFile == (HANDLE)(uint32_t)-11) fd = 1;
    else if (hFile == (HANDLE)(uint32_t)-12) fd = 2;

    if (!lpFileInformation) return 0;

    if (info_class == FileStandardInfo) {
        if (dwBufferSize < sizeof(FILE_STANDARD_INFO)) return 0;
        FILE_STANDARD_INFO fsi;
        memset(&fsi, 0, sizeof(fsi));

        // Get size via lseek
        off_t cur = lseek(fd, 0, SEEK_CUR);
        off_t size = lseek(fd, 0, SEEK_END);
        lseek(fd, cur, SEEK_SET);

        fsi.AllocationSize.QuadPart = (size + 4095) & ~4095;
        fsi.EndOfFile.QuadPart = size;
        fsi.NumberOfLinks = 1;
        fsi.DeletePending = FALSE;
        fsi.Directory = FALSE;

        mem_write(lpFileInformation, &fsi, sizeof(fsi));
        return 1;
    } else if (info_class == FileBasicInfo) {
        if (dwBufferSize < sizeof(FILE_BASIC_INFO)) return 0;
        FILE_BASIC_INFO fbi;
        memset(&fbi, 0, sizeof(fbi));
        fbi.FileAttributes = 0x80; // FILE_ATTRIBUTE_NORMAL
        mem_write(lpFileInformation, &fbi, sizeof(fbi));
        return 1;
    } else if (info_class == FileIdInfo) {
        if (dwBufferSize < sizeof(FILE_ID_INFO)) return 0;
        FILE_ID_INFO fii;
        memset(&fii, 0, sizeof(fii));
        fii.VolumeSerialNumber = 0x12345678;
        fii.FileId[0] = (BYTE)(fd & 0xFF);
        mem_write(lpFileInformation, &fii, sizeof(fii));
        return 1;
    }

    return 1;
}

static uint32_t hle_SetFilePointerEx(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFile = (HANDLE)args[0];
    LARGE_INTEGER distance;
    distance.u.LowPart = args[1];
    distance.u.HighPart = (LONG)args[2];
    DWORD new_pos_ptr = args[3];
    DWORD dwMoveMethod = args[4];

    void* native_fd = handle_get(hFile, HANDLE_TYPE_FILE);
    int fd = (int)(intptr_t)native_fd;

    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;

    off_t new_pos = lseek(fd, (off_t)distance.QuadPart, whence);
    if (new_pos == (off_t)-1) return 0;

    if (new_pos_ptr) {
        mem_write_u32(new_pos_ptr, (DWORD)(new_pos & 0xFFFFFFFF));
        mem_write_u32(new_pos_ptr + 4, (DWORD)(new_pos >> 32));
    }
    return 1;
}

static uint32_t hle_GetFinalPathNameByHandleW(struct CPUState* cpu, uint32_t* args) {
    HANDLE hFile = (HANDLE)args[0];
    DWORD filePath_ptr = args[1];
    DWORD cchFilePath = args[2];
    DWORD dwFlags = args[3];

    static const WCHAR dummy_path[] = {'C', ':', '\\', 'a', 'p', 'p', '.', 'e', 'x', 'e', 0};
    DWORD len = sizeof(dummy_path) / sizeof(WCHAR) - 1;

    if (cchFilePath > len && filePath_ptr) {
        mem_write(filePath_ptr, dummy_path, sizeof(dummy_path));
    }
    return len;
}

static uint32_t hle_CreateFile2(struct CPUState* cpu, uint32_t* args) {
    // Windows 8/10 CreateFile2: lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams
    extern uint32_t hle_CreateFileW_impl(DWORD name_ptr, DWORD access, DWORD share, DWORD disp);
    return hle_CreateFileW_impl(args[0], args[1], args[2], args[3]);
}

// -------------------------------------------------------------
// Vectored Exception Handling (VEH)
// -------------------------------------------------------------

#define MAX_VEH_HANDLERS 16
static DWORD s_veh_handlers[MAX_VEH_HANDLERS] = {0};
static uint32_t s_veh_count = 0;

static uint32_t hle_AddVectoredExceptionHandler(struct CPUState* cpu, uint32_t* args) {
    DWORD first = args[0];
    DWORD handler_pfn = args[1];

    if (s_veh_count < MAX_VEH_HANDLERS) {
        if (first) {
            // Insert at front
            for (uint32_t i = s_veh_count; i > 0; i--) {
                s_veh_handlers[i] = s_veh_handlers[i - 1];
            }
            s_veh_handlers[0] = handler_pfn;
        } else {
            s_veh_handlers[s_veh_count] = handler_pfn;
        }
        s_veh_count++;
        return handler_pfn; // Return non-null handle
    }
    return 0;
}

static uint32_t hle_RemoveVectoredExceptionHandler(struct CPUState* cpu, uint32_t* args) {
    DWORD handle = args[0];
    for (uint32_t i = 0; i < s_veh_count; i++) {
        if (s_veh_handlers[i] == handle) {
            for (uint32_t j = i; j < s_veh_count - 1; j++) {
                s_veh_handlers[j] = s_veh_handlers[j + 1];
            }
            s_veh_count--;
            return 1;
        }
    }
    return 0;
}

static uint32_t hle_SetUnhandledExceptionFilter(struct CPUState* cpu, uint32_t* args) {
    return 0; // Return previous filter (NULL)
}

static uint32_t hle_UnhandledExceptionFilter(struct CPUState* cpu, uint32_t* args) {
    return 0; // EXCEPTION_CONTINUE_SEARCH
}

// -------------------------------------------------------------
// Windows 7 Processor Groups & Thread APIs
// -------------------------------------------------------------

static uint32_t hle_GetActiveProcessorCount(struct CPUState* cpu, uint32_t* args) {
    return 2; // Report 2 active logical processors
}

static uint32_t hle_GetMaximumProcessorCount(struct CPUState* cpu, uint32_t* args) {
    return 2;
}

static uint32_t hle_GetActiveProcessorGroupCount(struct CPUState* cpu, uint32_t* args) {
    return 1; // 1 processor group on 32-bit hardware
}

static uint32_t hle_SetThreadDescription(struct CPUState* cpu, uint32_t* args) {
    // Windows 10 (1607+) named thread API
    return 0; // S_OK
}

static uint32_t hle_GetThreadDescription(struct CPUState* cpu, uint32_t* args) {
    return 0;
}

static uint32_t hle_GetThreadId(struct CPUState* cpu, uint32_t* args) {
    return 1001;
}

static uint32_t hle_GetProcessId(struct CPUState* cpu, uint32_t* args) {
    return 1000;
}

static uint32_t hle_SwitchToThread(struct CPUState* cpu, uint32_t* args) {
    sched_yield();
    return 1;
}

// -------------------------------------------------------------
// Synchronization: Events, Mutexes, and Semaphores (Ex APIs)
// -------------------------------------------------------------

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool manual_reset;
    bool signaled;
} WinEvent;

typedef struct {
    pthread_mutex_t mutex;
    DWORD owner_thread;
    uint32_t lock_count;
} WinMutex;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    LONG current_count;
    LONG maximum_count;
} WinSemaphore;

static uint32_t hle_CreateEventExW(struct CPUState* cpu, uint32_t* args) {
    DWORD security = args[0];
    DWORD name_ptr = args[1];
    DWORD flags = args[2];
    DWORD access = args[3];

    WinEvent* ev = (WinEvent*)malloc(sizeof(WinEvent));
    pthread_mutex_init(&ev->mutex, NULL);
    pthread_cond_init(&ev->cond, NULL);
    ev->manual_reset = (flags & 1) != 0; // CREATE_EVENT_MANUAL_RESET
    ev->signaled = (flags & 2) != 0;     // CREATE_EVENT_INITIAL_SET

    return (uint32_t)handle_alloc(ev, HANDLE_TYPE_EVENT);
}

static uint32_t hle_SetEvent(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    WinEvent* ev = (WinEvent*)handle_get(h, HANDLE_TYPE_EVENT);
    if (!ev) return 0;

    pthread_mutex_lock(&ev->mutex);
    ev->signaled = true;
    if (ev->manual_reset) pthread_cond_broadcast(&ev->cond);
    else pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->mutex);
    return 1;
}

static uint32_t hle_ResetEvent(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    WinEvent* ev = (WinEvent*)handle_get(h, HANDLE_TYPE_EVENT);
    if (!ev) return 0;

    pthread_mutex_lock(&ev->mutex);
    ev->signaled = false;
    pthread_mutex_unlock(&ev->mutex);
    return 1;
}

static uint32_t hle_CreateMutexExW(struct CPUState* cpu, uint32_t* args) {
    DWORD security = args[0];
    DWORD name_ptr = args[1];
    DWORD flags = args[2];
    DWORD access = args[3];

    WinMutex* mtx = (WinMutex*)malloc(sizeof(WinMutex));
    pthread_mutex_init(&mtx->mutex, NULL);
    mtx->owner_thread = (flags & 1) ? 1001 : 0; // CREATE_MUTEX_INITIAL_OWNER
    mtx->lock_count = (flags & 1) ? 1 : 0;

    return (uint32_t)handle_alloc(mtx, HANDLE_TYPE_MUTEX);
}

static uint32_t hle_ReleaseMutex(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    WinMutex* mtx = (WinMutex*)handle_get(h, HANDLE_TYPE_MUTEX);
    if (!mtx) return 0;

    pthread_mutex_lock(&mtx->mutex);
    if (mtx->lock_count > 0) {
        mtx->lock_count--;
        if (mtx->lock_count == 0) mtx->owner_thread = 0;
    }
    pthread_mutex_unlock(&mtx->mutex);
    return 1;
}

static uint32_t hle_CreateSemaphoreExW(struct CPUState* cpu, uint32_t* args) {
    DWORD security = args[0];
    LONG initial_count = (LONG)args[1];
    LONG max_count = (LONG)args[2];
    DWORD name_ptr = args[3];

    WinSemaphore* sem = (WinSemaphore*)malloc(sizeof(WinSemaphore));
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->current_count = initial_count;
    sem->maximum_count = max_count;

    return (uint32_t)handle_alloc(sem, HANDLE_TYPE_SEMAPHORE);
}

static uint32_t hle_ReleaseSemaphore(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    LONG release_count = (LONG)args[1];
    DWORD prev_count_ptr = args[2];

    WinSemaphore* sem = (WinSemaphore*)handle_get(h, HANDLE_TYPE_SEMAPHORE);
    if (!sem) return 0;

    pthread_mutex_lock(&sem->mutex);
    if (prev_count_ptr) mem_write_u32(prev_count_ptr, sem->current_count);
    sem->current_count += release_count;
    if (sem->current_count > sem->maximum_count) sem->current_count = sem->maximum_count;
    pthread_cond_broadcast(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
    return 1;
}

static uint32_t hle_WaitForSingleObjectEx(struct CPUState* cpu, uint32_t* args) {
    HANDLE h = (HANDLE)args[0];
    DWORD ms = args[1];
    BOOL alertable = args[2];

    WinEvent* ev = (WinEvent*)handle_get(h, HANDLE_TYPE_EVENT);
    if (ev) {
        pthread_mutex_lock(&ev->mutex);
        if (ev->signaled) {
            if (!ev->manual_reset) ev->signaled = false;
            pthread_mutex_unlock(&ev->mutex);
            return 0; // WAIT_OBJECT_0
        }
        pthread_mutex_unlock(&ev->mutex);
    }

    WinMutex* mtx = (WinMutex*)handle_get(h, HANDLE_TYPE_MUTEX);
    if (mtx) {
        pthread_mutex_lock(&mtx->mutex);
        if (mtx->lock_count == 0 || mtx->owner_thread == 1001) {
            mtx->lock_count++;
            mtx->owner_thread = 1001;
            pthread_mutex_unlock(&mtx->mutex);
            return 0; // WAIT_OBJECT_0
        }
        pthread_mutex_unlock(&mtx->mutex);
    }

    return 0; // WAIT_OBJECT_0
}

static uint32_t hle_WaitForMultipleObjectsEx(struct CPUState* cpu, uint32_t* args) {
    return 0; // WAIT_OBJECT_0
}

// -------------------------------------------------------------
// Cryptography: BCrypt & ProcessPrng (Windows 7 / 10)
// -------------------------------------------------------------

static uint32_t hle_BCryptGenRandom(struct CPUState* cpu, uint32_t* args) {
    DWORD hAlgorithm = args[0];
    DWORD pbBuffer = args[1];
    DWORD cbBuffer = args[2];
    DWORD dwFlags = args[3];

    if (!pbBuffer || cbBuffer == 0) return 0; // STATUS_SUCCESS

    void* buf = mem_to_host(pbBuffer);
    if (buf) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, cbBuffer);
            close(fd);
        }
    }
    return 0; // STATUS_SUCCESS
}

static uint32_t hle_ProcessPrng(struct CPUState* cpu, uint32_t* args) {
    DWORD pbBuffer = args[0];
    DWORD cbBuffer = args[1];
    return hle_BCryptGenRandom(cpu, args);
}

static uint32_t hle_BCryptOpenAlgorithmProvider(struct CPUState* cpu, uint32_t* args) {
    DWORD phAlgorithm = args[0];
    if (phAlgorithm) mem_write_u32(phAlgorithm, 0xBC01);
    return 0; // STATUS_SUCCESS
}

static uint32_t hle_BCryptCloseAlgorithmProvider(struct CPUState* cpu, uint32_t* args) {
    return 0; // STATUS_SUCCESS
}

// -------------------------------------------------------------
// Native NT APIs (ntdll.dll)
// -------------------------------------------------------------

static uint32_t hle_NtQueryInformationProcess(struct CPUState* cpu, uint32_t* args) {
    DWORD hProcess = args[0];
    DWORD process_info_class = args[1];
    DWORD pProcessInfo = args[2];
    DWORD length = args[3];
    DWORD pReturnLength = args[4];

    if (!pProcessInfo) return 0xC0000004; // STATUS_INFO_LENGTH_MISMATCH

    if (process_info_class == 0) { // ProcessBasicInformation
        if (length < 24) return 0xC0000004;
        // struct PROCESS_BASIC_INFORMATION { ExitStatus, PebBaseAddress, AffinityMask, BasePriority, UniqueProcessId, InheritedFromUniqueProcessId }
        mem_write_u32(pProcessInfo + 0, 0); // ExitStatus
        mem_write_u32(pProcessInfo + 4, DEFAULT_PEB_ADDRESS); // PebBaseAddress
        mem_write_u32(pProcessInfo + 8, 3); // AffinityMask
        mem_write_u32(pProcessInfo + 12, 8); // BasePriority
        mem_write_u32(pProcessInfo + 16, 1000); // UniqueProcessId
        mem_write_u32(pProcessInfo + 20, 999);  // InheritedFromUniqueProcessId
        if (pReturnLength) mem_write_u32(pReturnLength, 24);
        return 0; // STATUS_SUCCESS
    }
    return 0;
}

static uint32_t hle_NtQuerySystemInformation(struct CPUState* cpu, uint32_t* args) {
    DWORD sys_info_class = args[0];
    DWORD pSystemInfo = args[1];
    DWORD length = args[2];
    DWORD pReturnLength = args[3];

    if (pReturnLength) mem_write_u32(pReturnLength, length);
    return 0; // STATUS_SUCCESS
}

static uint32_t hle_RtlCaptureContext(struct CPUState* cpu, uint32_t* args) {
    DWORD ctx_ptr = args[0];
    if (!ctx_ptr) return 0;

    CONTEXT32 ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.Eax = cpu->eax;
    ctx.Ecx = cpu->ecx;
    ctx.Edx = cpu->edx;
    ctx.Ebx = cpu->ebx;
    ctx.Esp = cpu->esp;
    ctx.Ebp = cpu->ebp;
    ctx.Esi = cpu->esi;
    ctx.Edi = cpu->edi;
    ctx.Eip = cpu->eip;
    ctx.EFlags = cpu->eflags;

    mem_write(ctx_ptr, &ctx, sizeof(ctx));
    return 0;
}

// Windows 8 / 10 Delay Load & ETW / WinRT APIs
static uint32_t hle_ResolveDelayLoadedAPI(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t parent_base = args[0];
    uint32_t delay_desc = args[1];
    uint32_t thunk_addr = args[4];

    uint32_t dll_name_rva = mem_read_u32(delay_desc + 4);
    const char* dll_name = (const char*)mem_to_host(parent_base + dll_name_rva);
    const char* resolved_dll = apiset_resolve(dll_name);

    uint32_t iat_rva = mem_read_u32(delay_desc + 12);
    uint32_t int_rva = mem_read_u32(delay_desc + 16);

    uint32_t idx = (thunk_addr - (parent_base + iat_rva)) / 4;
    uint32_t thunk_val = mem_read_u32(parent_base + int_rva + idx * 4);

    const char* func_name = NULL;
    uint32_t ordinal = 0;
    if (IMAGE_SNAP_BY_ORDINAL32(thunk_val)) {
        ordinal = IMAGE_ORDINAL32(thunk_val);
    } else {
        func_name = (const char*)mem_to_host(parent_base + thunk_val + 2);
    }

    uint32_t target_thunk = hle_resolve(resolved_dll, func_name, ordinal);
    if (!target_thunk) {
        target_thunk = hle_register_stub(resolved_dll, func_name, ordinal);
    }

    mem_write_u32(thunk_addr, target_thunk);
    return target_thunk;
}

static uint32_t hle_DelayLoadFailureHook(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_EventRegister(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t reg_handle_ptr = args[3];
    if (reg_handle_ptr) {
        mem_write_u32(reg_handle_ptr, 0x1234);
        mem_write_u32(reg_handle_ptr + 4, 0);
    }
    return 0; // ERROR_SUCCESS
}

static uint32_t hle_EventUnregister(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_EventSetInformation(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_EventWriteTransfer(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_EventProviderEnabled(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_RoInitialize(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // S_OK
}

static uint32_t hle_RoUninitialize(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_RoGetActivationFactory(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[2]) mem_write_u32(args[2], 0);
    return 0x80004002; // E_NOINTERFACE
}

static uint32_t hle_RoGetMatchingRestrictedErrorInfo(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_SetRestrictedErrorInfo(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_WindowsCreateString(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t out_hstr = args[2];
    if (out_hstr) mem_write_u32(out_hstr, args[0]);
    return 0; // S_OK
}

static uint32_t hle_WindowsCreateStringReference(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t out_hstr = args[2];
    if (out_hstr) mem_write_u32(out_hstr, args[0]);
    return 0; // S_OK
}

static uint32_t hle_WindowsDeleteString(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_WindowsGetStringRawBuffer(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[1]) mem_write_u32(args[1], 0);
    return args[0];
}

static uint32_t hle_CoInitializeEx(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // S_OK
}

static uint32_t hle_CoUninitialize(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_CoCreateInstance(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t ppv = args[4];
    if (ppv) mem_write_u32(ppv, 0);
    return 0x80004002; // E_NOINTERFACE
}

static uint32_t hle_CoTaskMemAlloc(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t cb = args[0];
    return mem_virtual_alloc(0, cb, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static uint32_t hle_CoTaskMemFree(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t ptr = args[0];
    if (ptr) mem_virtual_free(ptr, 0, MEM_RELEASE);
    return 0;
}

static uint32_t hle_CoCreateFreeThreadedMarshaler(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[1]) mem_write_u32(args[1], 0);
    return 0;
}

static uint32_t hle_CoWaitForMultipleHandles(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_PropVariantClear(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[0]) memset(mem_to_host(args[0]), 0, 16);
    return 0;
}

static uint32_t hle_CoCreateGuid(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[0]) memset(mem_to_host(args[0]), 0x42, 16);
    return 0;
}

// Advapi32 Registry APIs
static uint32_t hle_RegOpenKeyExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t phkResult = args[4];
    if (phkResult) mem_write_u32(phkResult, 0x80000001);
    return 0; // ERROR_SUCCESS
}

static uint32_t hle_RegQueryValueExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t lpType = args[3];
    uint32_t lpcbData = args[5];
    if (lpcbData) mem_write_u32(lpcbData, 0);
    if (lpType) mem_write_u32(lpType, 0);
    return 2; // ERROR_FILE_NOT_FOUND (forces application to use default configuration)
}

static uint32_t hle_RegCloseKey(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // ERROR_SUCCESS
}

static uint32_t hle_RegCreateKeyExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t phkResult = args[7];
    uint32_t lpdwDisposition = args[8];
    if (phkResult) mem_write_u32(phkResult, 0x80000001);
    if (lpdwDisposition) mem_write_u32(lpdwDisposition, 1); // REG_CREATED_NEW_KEY
    return 0;
}

static uint32_t hle_RegCreateKeyW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t phkResult = args[2];
    if (phkResult) mem_write_u32(phkResult, 0x80000001);
    return 0;
}

static uint32_t hle_RegSetValueExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // ERROR_SUCCESS
}

static uint32_t hle_RegDeleteKeyExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_RegQueryInfoKeyW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[4]) mem_write_u32(args[4], 0); // lpcSubKeys
    if (args[7]) mem_write_u32(args[7], 0); // lpcValues
    return 0;
}

static uint32_t hle_RegEnumValueW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 259; // ERROR_NO_MORE_ITEMS
}

// Advapi32 Security & Tokens
static uint32_t hle_OpenProcessToken(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[2]) mem_write_u32(args[2], 0x9001);
    return 1; // TRUE
}

static uint32_t hle_GetTokenInformation(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[4]) mem_write_u32(args[4], 0);
    return 0; // FALSE
}

static uint32_t hle_IsTextUnicode(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_DuplicateEncryptionInfoFile(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_DecryptFileW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

// Shell & Shcore
static uint32_t hle_SHStrDupW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t psz = args[0];
    uint32_t ppwsz = args[1];
    if (!ppwsz) return 0x80070057; // E_INVALIDARG
    if (!psz) {
        mem_write_u32(ppwsz, 0);
        return 0;
    }
    const uint16_t* host_str = (const uint16_t*)mem_to_host(psz);
    if (!host_str) {
        mem_write_u32(ppwsz, 0);
        return 0;
    }
    size_t len = 0;
    while (host_str[len]) len++;
    size_t bytes = (len + 1) * 2;
    uint32_t out = mem_virtual_alloc(0, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!out) return 0x8007000E; // E_OUTOFMEMORY
    mem_write(out, host_str, bytes);
    mem_write_u32(ppwsz, out);
    return 0; // S_OK
}

static uint32_t hle_PathFindExtensionW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t psz = args[0];
    if (!psz) return 0;
    const uint16_t* host_str = (const uint16_t*)mem_to_host(psz);
    if (!host_str) return psz;
    uint32_t last_dot = 0;
    for (uint32_t i = 0; host_str[i]; i++) {
        if (host_str[i] == L'.') last_dot = psz + i * 2;
    }
    if (last_dot) return last_dot;
    size_t len = 0;
    while (host_str[len]) len++;
    return psz + (uint32_t)(len * 2);
}

static uint32_t hle_PathFileExistsW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // FALSE
}

static uint32_t hle_PathIsFileSpecW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1; // TRUE
}

static uint32_t hle_SHGetKnownFolderPath(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t ppszPath = args[3];
    if (ppszPath) {
        static const uint16_t s_docs[] = { 'C',':','\\','U','s','e','r','s','\\','D','o','c','u','m','e','n','t','s',0 };
        uint32_t out = mem_virtual_alloc(0, sizeof(s_docs), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (out) {
            mem_write(out, s_docs, sizeof(s_docs));
            mem_write_u32(ppszPath, out);
            return 0; // S_OK
        }
    }
    return 0x80004005; // E_FAIL
}

static uint32_t hle_SHCreateItemFromParsingName(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[3]) mem_write_u32(args[3], 0);
    return 0x80070002; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
}

static uint32_t hle_PropVariantToStringVectorAlloc(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x80004005;
}

static uint32_t hle_PSGetPropertyDescriptionListFromString(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x80004005;
}

static uint32_t hle_SetWindowSubclass(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1; // TRUE
}


static uint32_t hle_GetProcessMitigationPolicy(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[2] && args[3]) memset(mem_to_host(args[2]), 0, args[3]);
    return 1;
}

static uint32_t hle_CreateThreadpoolTimer(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x5001; // dummy timer handle
}

static uint32_t hle_SetThreadpoolTimer(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_WaitForThreadpoolTimerCallbacks(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_CloseThreadpoolTimer(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_RegisterApplicationRestart(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

// -------------------------------------------------------------
// Registration Helper for all Win7 / Win10 APIs
// -------------------------------------------------------------

void register_win7_10_syscalls(void) {
    // Memory Status (Accurate 512MB RAM representation)
    hle_register("kernel32.dll", "GlobalMemoryStatusEx", 0, hle_GlobalMemoryStatusEx, 1, false);

    // SRW Locks (Vista / 7 / 10)
    hle_register("kernel32.dll", "InitializeSRWLock", 0, hle_InitializeSRWLock, 1, false);
    hle_register("kernel32.dll", "AcquireSRWLockExclusive", 0, hle_AcquireSRWLockExclusive, 1, false);
    hle_register("kernel32.dll", "ReleaseSRWLockExclusive", 0, hle_ReleaseSRWLockExclusive, 1, false);
    hle_register("kernel32.dll", "AcquireSRWLockShared", 0, hle_AcquireSRWLockShared, 1, false);
    hle_register("kernel32.dll", "ReleaseSRWLockShared", 0, hle_ReleaseSRWLockShared, 1, false);
    hle_register("kernel32.dll", "TryAcquireSRWLockExclusive", 0, hle_TryAcquireSRWLockExclusive, 1, false);
    hle_register("kernel32.dll", "TryAcquireSRWLockShared", 0, hle_TryAcquireSRWLockShared, 1, false);

    // Condition Variables (Vista / 7 / 10)
    hle_register("kernel32.dll", "InitializeConditionVariable", 0, hle_InitializeConditionVariable, 1, false);
    hle_register("kernel32.dll", "WakeConditionVariable", 0, hle_WakeConditionVariable, 1, false);
    hle_register("kernel32.dll", "WakeAllConditionVariable", 0, hle_WakeAllConditionVariable, 1, false);
    hle_register("kernel32.dll", "SleepConditionVariableSRW", 0, hle_SleepConditionVariableSRW, 4, false);

    // InitOnce (Vista / 7 / 10)
    hle_register("kernel32.dll", "InitOnceInitialize", 0, hle_InitOnceInitialize, 1, false);
    hle_register("kernel32.dll", "InitOnceBeginInitialize", 0, hle_InitOnceBeginInitialize, 4, false);
    hle_register("kernel32.dll", "InitOnceComplete", 0, hle_InitOnceComplete, 3, false);

    // WaitOnAddress / Futex (Win 8 / 10)
    hle_register("kernel32.dll", "WaitOnAddress", 0, hle_WaitOnAddress, 4, false);
    hle_register("kernel32.dll", "WakeByAddressSingle", 0, hle_WakeByAddressSingle, 1, false);
    hle_register("kernel32.dll", "WakeByAddressAll", 0, hle_WakeByAddressAll, 1, false);

    // High Precision Time (Win 8 / 10)
    hle_register("kernel32.dll", "GetSystemTimePreciseAsFileTime", 0, hle_GetSystemTimePreciseAsFileTime, 1, false);
    hle_register("kernel32.dll", "GetTickCount64", 0, hle_GetTickCount64, 0, false);

    // Extended Filesystem & FileInfo (Win Vista / 7 / 8 / 10)
    hle_register("kernel32.dll", "GetFileInformationByHandleEx", 0, hle_GetFileInformationByHandleEx, 4, false);
    hle_register("kernel32.dll", "SetFilePointerEx", 0, hle_SetFilePointerEx, 5, false);
    hle_register("kernel32.dll", "GetFinalPathNameByHandleW", 0, hle_GetFinalPathNameByHandleW, 4, false);
    hle_register("kernel32.dll", "CreateFile2", 0, hle_CreateFile2, 5, false);

    // Vectored Exception Handling & Filters
    hle_register("kernel32.dll", "AddVectoredExceptionHandler", 0, hle_AddVectoredExceptionHandler, 2, false);
    hle_register("kernel32.dll", "RemoveVectoredExceptionHandler", 0, hle_RemoveVectoredExceptionHandler, 1, false);
    hle_register("kernel32.dll", "SetUnhandledExceptionFilter", 0, hle_SetUnhandledExceptionFilter, 1, false);
    hle_register("kernel32.dll", "UnhandledExceptionFilter", 0, hle_UnhandledExceptionFilter, 1, false);

    // Processor Groups & Threads (Win 7 / 10)
    hle_register("kernel32.dll", "GetActiveProcessorCount", 0, hle_GetActiveProcessorCount, 1, false);
    hle_register("kernel32.dll", "GetMaximumProcessorCount", 0, hle_GetMaximumProcessorCount, 1, false);
    hle_register("kernel32.dll", "GetActiveProcessorGroupCount", 0, hle_GetActiveProcessorGroupCount, 0, false);
    hle_register("kernel32.dll", "SetThreadDescription", 0, hle_SetThreadDescription, 2, false);
    hle_register("kernel32.dll", "GetThreadDescription", 0, hle_GetThreadDescription, 2, false);
    hle_register("kernel32.dll", "GetThreadId", 0, hle_GetThreadId, 1, false);
    hle_register("kernel32.dll", "GetProcessId", 0, hle_GetProcessId, 1, false);
    hle_register("kernel32.dll", "SwitchToThread", 0, hle_SwitchToThread, 0, false);

    // Modern Sync: Events, Mutexes, Semaphores Ex
    hle_register("kernel32.dll", "CreateEventExW", 0, hle_CreateEventExW, 4, false);
    hle_register("kernel32.dll", "SetEvent", 0, hle_SetEvent, 1, false);
    hle_register("kernel32.dll", "ResetEvent", 0, hle_ResetEvent, 1, false);
    hle_register("kernel32.dll", "CreateMutexExW", 0, hle_CreateMutexExW, 4, false);
    hle_register("kernel32.dll", "ReleaseMutex", 0, hle_ReleaseMutex, 1, false);
    hle_register("kernel32.dll", "CreateSemaphoreExW", 0, hle_CreateSemaphoreExW, 4, false);
    hle_register("kernel32.dll", "ReleaseSemaphore", 0, hle_ReleaseSemaphore, 3, false);
    hle_register("kernel32.dll", "WaitForSingleObjectEx", 0, hle_WaitForSingleObjectEx, 3, false);
    hle_register("kernel32.dll", "WaitForMultipleObjectsEx", 0, hle_WaitForMultipleObjectsEx, 4, false);

    // Cryptography (bcrypt.dll & ProcessPrng)
    hle_register("bcrypt.dll", "BCryptGenRandom", 0, hle_BCryptGenRandom, 4, false);
    hle_register("bcrypt.dll", "BCryptOpenAlgorithmProvider", 0, hle_BCryptOpenAlgorithmProvider, 4, false);
    hle_register("bcrypt.dll", "BCryptCloseAlgorithmProvider", 0, hle_BCryptCloseAlgorithmProvider, 2, false);
    hle_register("bcryptprimitives.dll", "ProcessPrng", 0, hle_ProcessPrng, 2, false);

    // Extended System Info (Win 7 / 10)
    hle_register("kernel32.dll", "GetLogicalProcessorInformationEx", 0, hle_GetLogicalProcessorInformationEx, 3, false);
    hle_register("kernel32.dll", "GetProductInfo", 0, hle_GetProductInfo, 5, false);

    // Extended Memory (Win 7 / 8 / 10)
    hle_register("kernel32.dll", "VirtualAllocExNuma", 0, hle_VirtualAllocExNuma, 6, false);
    hle_register("kernel32.dll", "GetLargePageMinimum", 0, hle_GetLargePageMinimum, 0, false);
    hle_register("kernel32.dll", "PrefetchVirtualMemory", 0, hle_PrefetchVirtualMemory, 4, false);
    hle_register("kernel32.dll", "DiscardVirtualMemory", 0, hle_DiscardVirtualMemory, 2, false);

    // NLS & Localization (Win 7 / 10)
    hle_register("kernel32.dll", "GetUserDefaultLocaleName", 0, hle_GetUserDefaultLocaleName, 2, false);
    hle_register("kernel32.dll", "ResolveLocaleName", 0, hle_ResolveLocaleName, 3, false);
    hle_register("kernel32.dll", "GetLocaleInfoEx", 0, hle_GetLocaleInfoEx, 4, false);
    hle_register("kernel32.dll", "IdnToAscii", 0, hle_IdnToAscii, 5, false);

    // Threadpool (Vista / 7 / 10)
    hle_register("kernel32.dll", "CreateThreadpoolWork", 0, hle_CreateThreadpoolWork, 3, false);
    hle_register("kernel32.dll", "SubmitThreadpoolWork", 0, hle_SubmitThreadpoolWork, 1, false);
    hle_register("kernel32.dll", "CloseThreadpoolWork", 0, hle_CloseThreadpoolWork, 1, false);
    hle_register("kernel32.dll", "WaitForThreadpoolWorkCallbacks", 0, hle_WaitForThreadpoolWorkCallbacks, 2, false);

    // Native NT APIs (ntdll.dll)
    hle_register("ntdll.dll", "NtQueryInformationProcess", 0, hle_NtQueryInformationProcess, 5, false);
    hle_register("ntdll.dll", "NtQuerySystemInformation", 0, hle_NtQuerySystemInformation, 4, false);
    hle_register("ntdll.dll", "RtlCaptureContext", 0, hle_RtlCaptureContext, 1, false);

    // Windows 8 / 10 Delay Load
    hle_register("kernel32.dll", "ResolveDelayLoadedAPI", 0, hle_ResolveDelayLoadedAPI, 6, false);
    hle_register("kernel32.dll", "DelayLoadFailureHook", 0, hle_DelayLoadFailureHook, 2, false);

    // Event Tracing for Windows (ETW / advapi32.dll)
    hle_register("advapi32.dll", "EventRegister", 0, hle_EventRegister, 4, false);
    hle_register("advapi32.dll", "EventUnregister", 0, hle_EventUnregister, 2, false);
    hle_register("advapi32.dll", "EventSetInformation", 0, hle_EventSetInformation, 5, false);
    hle_register("advapi32.dll", "EventWriteTransfer", 0, hle_EventWriteTransfer, 7, false);
    hle_register("advapi32.dll", "EventProviderEnabled", 0, hle_EventProviderEnabled, 5, false);

    // Modern Threadpool & Process Mitigation (kernel32.dll)
    hle_register("kernel32.dll", "GetProcessMitigationPolicy", 0, hle_GetProcessMitigationPolicy, 4, false);
    hle_register("kernel32.dll", "CreateThreadpoolTimer", 0, hle_CreateThreadpoolTimer, 3, false);
    hle_register("kernel32.dll", "SetThreadpoolTimer", 0, hle_SetThreadpoolTimer, 4, false);
    hle_register("kernel32.dll", "WaitForThreadpoolTimerCallbacks", 0, hle_WaitForThreadpoolTimerCallbacks, 2, false);
    hle_register("kernel32.dll", "CloseThreadpoolTimer", 0, hle_CloseThreadpoolTimer, 1, false);
    hle_register("kernel32.dll", "RegisterApplicationRestart", 0, hle_RegisterApplicationRestart, 2, false);

    // WinRT & COM Runtime
    hle_register("api-ms-win-core-winrt-l1-1-0.dll", "RoInitialize", 0, hle_RoInitialize, 1, false);
    hle_register("api-ms-win-core-winrt-l1-1-0.dll", "RoUninitialize", 0, hle_RoUninitialize, 0, false);
    hle_register("api-ms-win-core-winrt-l1-1-0.dll", "RoGetActivationFactory", 0, hle_RoGetActivationFactory, 3, false);
    hle_register("api-ms-win-core-winrt-error-l1-1-1.dll", "RoGetMatchingRestrictedErrorInfo", 0, hle_RoGetMatchingRestrictedErrorInfo, 2, false);
    hle_register("api-ms-win-core-winrt-error-l1-1-0.dll", "SetRestrictedErrorInfo", 0, hle_SetRestrictedErrorInfo, 1, false);
    hle_register("api-ms-win-core-winrt-string-l1-1-0.dll", "WindowsCreateString", 0, hle_WindowsCreateString, 3, false);
    hle_register("api-ms-win-core-winrt-string-l1-1-0.dll", "WindowsCreateStringReference", 0, hle_WindowsCreateStringReference, 4, false);
    hle_register("api-ms-win-core-winrt-string-l1-1-0.dll", "WindowsDeleteString", 0, hle_WindowsDeleteString, 1, false);
    hle_register("api-ms-win-core-winrt-string-l1-1-0.dll", "WindowsGetStringRawBuffer", 0, hle_WindowsGetStringRawBuffer, 2, false);
    hle_register("ole32.dll", "CoInitializeEx", 0, hle_CoInitializeEx, 2, false);
    hle_register("ole32.dll", "CoUninitialize", 0, hle_CoUninitialize, 0, false);
    hle_register("ole32.dll", "CoCreateInstance", 0, hle_CoCreateInstance, 5, false);
    hle_register("ole32.dll", "CoTaskMemAlloc", 0, hle_CoTaskMemAlloc, 1, false);
    hle_register("ole32.dll", "CoTaskMemFree", 0, hle_CoTaskMemFree, 1, false);
    hle_register("ole32.dll", "CoCreateFreeThreadedMarshaler", 0, hle_CoCreateFreeThreadedMarshaler, 2, false);
    hle_register("ole32.dll", "CoWaitForMultipleHandles", 0, hle_CoWaitForMultipleHandles, 5, false);
    hle_register("ole32.dll", "PropVariantClear", 0, hle_PropVariantClear, 1, false);
    hle_register("ole32.dll", "CoCreateGuid", 0, hle_CoCreateGuid, 1, false);

    // Registry APIs (advapi32.dll)
    hle_register("advapi32.dll", "RegOpenKeyExW", 0, hle_RegOpenKeyExW, 5, false);
    hle_register("advapi32.dll", "RegQueryValueExW", 0, hle_RegQueryValueExW, 6, false);
    hle_register("advapi32.dll", "RegCloseKey", 0, hle_RegCloseKey, 1, false);
    hle_register("advapi32.dll", "RegCreateKeyExW", 0, hle_RegCreateKeyExW, 9, false);
    hle_register("advapi32.dll", "RegCreateKeyW", 0, hle_RegCreateKeyW, 3, false);
    hle_register("advapi32.dll", "RegSetValueExW", 0, hle_RegSetValueExW, 6, false);
    hle_register("advapi32.dll", "RegDeleteKeyExW", 0, hle_RegDeleteKeyExW, 4, false);
    hle_register("advapi32.dll", "RegQueryInfoKeyW", 0, hle_RegQueryInfoKeyW, 12, false);
    hle_register("advapi32.dll", "RegEnumValueW", 0, hle_RegEnumValueW, 8, false);

    // Security & Token APIs
    hle_register("advapi32.dll", "OpenProcessToken", 0, hle_OpenProcessToken, 3, false);
    hle_register("advapi32.dll", "GetTokenInformation", 0, hle_GetTokenInformation, 5, false);
    hle_register("advapi32.dll", "IsTextUnicode", 0, hle_IsTextUnicode, 3, false);
    hle_register("advapi32.dll", "DuplicateEncryptionInfoFile", 0, hle_DuplicateEncryptionInfoFile, 2, false);
    hle_register("advapi32.dll", "DecryptFileW", 0, hle_DecryptFileW, 2, false);

    // Shell & Shcore APIs
    hle_register("shcore.dll", "SHStrDupW", 0, hle_SHStrDupW, 2, false);
    hle_register("shlwapi.dll", "PathFindExtensionW", 0, hle_PathFindExtensionW, 1, false);
    hle_register("shlwapi.dll", "PathFileExistsW", 0, hle_PathFileExistsW, 1, false);
    hle_register("shlwapi.dll", "PathIsFileSpecW", 0, hle_PathIsFileSpecW, 1, false);
    hle_register("shell32.dll", "SHGetKnownFolderPath", 0, hle_SHGetKnownFolderPath, 4, false);
    hle_register("shell32.dll", "SHCreateItemFromParsingName", 0, hle_SHCreateItemFromParsingName, 4, false);
    hle_register("propsys.dll", "PropVariantToStringVectorAlloc", 0, hle_PropVariantToStringVectorAlloc, 3, false);
    hle_register("propsys.dll", "PSGetPropertyDescriptionListFromString", 0, hle_PSGetPropertyDescriptionListFromString, 3, false);

    // COMCTL32 Subclassing
    hle_register("comctl32.dll", "SetWindowSubclass", 345, hle_SetWindowSubclass, 4, false);
}
