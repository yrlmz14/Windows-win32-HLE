#include "teb_peb.h"
#include "memory.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

void init_teb_peb_shared_data(WindowsVersionProfile profile, DWORD image_base) {
    // 1. Map KUSER_SHARED_DATA at 0x7FFE0000
    mem_virtual_alloc(KUSER_SHARED_DATA_ADDRESS, PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    KUSER_SHARED_DATA* kuser = (KUSER_SHARED_DATA*)mem_to_host(KUSER_SHARED_DATA_ADDRESS);
    if (kuser) {
        memset(kuser, 0, sizeof(KUSER_SHARED_DATA));
        kuser->TickCountMultiplier = 0x0FA00000;

        DWORD major = (profile == WIN_VER_10) ? 10 : 6;
        DWORD minor = (profile == WIN_VER_10) ? 0 : 1;
        DWORD build = (profile == WIN_VER_10) ? 19045 : 7601;

        kuser->NtMajorVersion = major;
        kuser->NtMinorVersion = minor;
        kuser->NtBuildNumber  = build;
        kuser->NtProductType  = 1; // VER_NT_WORKSTATION
        kuser->ProductTypeIsValid = 1;
        kuser->NativeProcessorArchitecture = 0; // PROCESSOR_ARCHITECTURE_INTEL

        // 512 MB RAM = 131,072 4KB pages
        kuser->NumberOfPhysicalPages = (512 * 1024 * 1024) / PAGE_SIZE_4K;

        // Processor features (PF_FLOATING_POINT_PRECISION_ERRATA, PF_MMX, PF_XMMI, PF_XMMI64, PF_COMPARE_EXCHANGE_DOUBLE)
        kuser->ProcessorFeatures[1] = 1; // PF_FLOATING_POINT_EMULATED = 0
        kuser->ProcessorFeatures[2] = 1; // PF_COMPARE_EXCHANGE_DOUBLE (CMPXCHG8B)
        kuser->ProcessorFeatures[3] = 1; // PF_MMX_INSTRUCTIONS_AVAILABLE
        kuser->ProcessorFeatures[6] = 1; // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
        kuser->ProcessorFeatures[10] = 1; // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
        kuser->ProcessorFeatures[13] = 1; // PF_SSE3_INSTRUCTIONS_AVAILABLE

        kuser->KdDebuggerEnabled = 0;
    }

    // 2. Map PEB at 0x7FFDF000
    mem_virtual_alloc(DEFAULT_PEB_ADDRESS, PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    PEB32* peb = (PEB32*)mem_to_host(DEFAULT_PEB_ADDRESS);
    if (peb) {
        memset(peb, 0, sizeof(PEB32));
        peb->BeingDebugged = 0;
        peb->ImageBaseAddress = image_base;
        peb->ProcessHeap = 0x00200000; // Will be initialized by heap manager
        peb->NumberOfProcessors = 2;

        DWORD major = (profile == WIN_VER_10) ? 10 : 6;
        DWORD minor = (profile == WIN_VER_10) ? 0 : 1;
        WORD  build = (profile == WIN_VER_10) ? 19045 : 7601;

        peb->OSMajorVersion = major;
        peb->OSMinorVersion = minor;
        peb->OSBuildNumber  = build;
        peb->OSPlatformId   = 2; // VER_PLATFORM_WIN32_NT
    }

    // 3. Map TEB at 0x7FFDE000
    mem_virtual_alloc(DEFAULT_TEB_ADDRESS, PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    TEB32* teb = (TEB32*)mem_to_host(DEFAULT_TEB_ADDRESS);
    if (teb) {
        memset(teb, 0, sizeof(TEB32));
        teb->StackBase = 0x00100000;
        teb->StackLimit = 0x000F0000;
        teb->Self = DEFAULT_TEB_ADDRESS;                 // FS:[0x18] points to TEB
        teb->ProcessEnvironmentBlock = DEFAULT_PEB_ADDRESS; // FS:[0x30] points to PEB
        teb->ClientIdUniqueProcess = 1000;
        teb->ClientIdUniqueThread  = 1001;
        teb->LastErrorValue = 0;

        // Initialize ThreadLocalStoragePointer (FS:[0x2C])
        DWORD tls_array = mem_virtual_alloc(0, PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD tls_slot0 = mem_virtual_alloc(0, PAGE_SIZE_4K, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (tls_array && tls_slot0) {
            mem_write_u32(tls_array, tls_slot0);
            teb->ThreadLocalStoragePointer = tls_array;
        }
    }

    update_kuser_shared_data_time();
}

void update_kuser_shared_data_time(void) {
    KUSER_SHARED_DATA* kuser = (KUSER_SHARED_DATA*)mem_to_host(KUSER_SHARED_DATA_ADDRESS);
    if (!kuser) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Convert UNIX epoch (Jan 1 1970) to Windows FILETIME (Jan 1 1601) in 100ns units
    // 11644473600 seconds difference
    uint64_t win_time = ((uint64_t)tv.tv_sec + 11644473600ULL) * 10000000ULL + ((uint64_t)tv.tv_usec * 10ULL);

    kuser->SystemTimeLow = (DWORD)(win_time & 0xFFFFFFFF);
    kuser->SystemTimeHigh1 = (DWORD)(win_time >> 32);
    kuser->SystemTimeHigh2 = kuser->SystemTimeHigh1;

    // Approximate tick count in ms
    uint64_t ms = ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
    kuser->TickCountLowDeprecated = (DWORD)ms;
}
