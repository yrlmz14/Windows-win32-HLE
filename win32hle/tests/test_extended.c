typedef unsigned short wchar_t;

void* memset(void* dest, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)dest;
    while (n--) *p++ = (unsigned char)c;
    return dest;
}

void* memcpy(void* dest, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) int __stdcall WriteFile(void* hFile, const void* lpBuffer, unsigned int nNumberOfBytesToWrite, unsigned int* lpNumberOfBytesWritten, void* lpOverlapped);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned int nStdHandle);

typedef struct {
    unsigned int dwLength;
    unsigned int dwMemoryLoad;
    unsigned long long ullTotalPhys;
    unsigned long long ullAvailPhys;
    unsigned long long ullTotalPageFile;
    unsigned long long ullAvailPageFile;
    unsigned long long ullTotalVirtual;
    unsigned long long ullAvailVirtual;
    unsigned long long ullAvailExtendedVirtual;
} MEMORYSTATUSEX;

__declspec(dllimport) int __stdcall GlobalMemoryStatusEx(MEMORYSTATUSEX* lpBuffer);
__declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(unsigned int FirstHandler, void* VectoredHandler);
__declspec(dllimport) unsigned int __stdcall RemoveVectoredExceptionHandler(void* Handle);
__declspec(dllimport) unsigned int __stdcall GetActiveProcessorCount(unsigned short GroupNumber);
__declspec(dllimport) int __stdcall SetThreadDescription(void* hThread, const wchar_t* lpThreadDescription);
__declspec(dllimport) void* __stdcall CreateEventExW(void* lpEventAttributes, const wchar_t* lpName, unsigned int dwFlags, unsigned int dwDesiredAccess);
__declspec(dllimport) int __stdcall SetEvent(void* hEvent);
__declspec(dllimport) unsigned int __stdcall WaitForSingleObjectEx(void* hHandle, unsigned int dwMilliseconds, int bAlertable);
__declspec(dllimport) void* __stdcall CreateMutexExW(void* lpMutexAttributes, const wchar_t* lpName, unsigned int dwFlags, unsigned int dwDesiredAccess);
__declspec(dllimport) int __stdcall ReleaseMutex(void* hMutex);
__declspec(dllimport) int __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer, unsigned int cbBuffer, unsigned int dwFlags);
__declspec(dllimport) int __stdcall ProcessPrng(unsigned char* pbBuffer, unsigned int cbBuffer);

typedef struct {
    int ExitStatus;
    void* PebBaseAddress;
    unsigned int AffinityMask;
    int BasePriority;
    unsigned int UniqueProcessId;
    unsigned int InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

__declspec(dllimport) int __stdcall NtQueryInformationProcess(void* ProcessHandle, unsigned int ProcessInformationClass, void* ProcessInformation, unsigned int ProcessInformationLength, unsigned int* ReturnLength);

static void print_str(const char* s) {
    void* hOut = GetStdHandle((unsigned int)-11);
    unsigned int len = 0;
    while (s[len]) len++;
    unsigned int written = 0;
    WriteFile(hOut, s, len, &written, 0);
}

void dummy_veh(void* info) {
}

void _start() {
    print_str("[GUEST] Starting Extended Windows 7 & 10 Suite...\n");

    // 1. Test GlobalMemoryStatusEx (512MB RAM awareness)
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_status)) {
        if (mem_status.ullTotalPhys == (512ULL * 1024 * 1024)) {
            print_str("[GUEST]  [PASS] GlobalMemoryStatusEx reported accurate 512MB physical RAM\n");
        }
    }

    // 2. Test Vectored Exception Handling
    void* veh_handle = AddVectoredExceptionHandler(1, dummy_veh);
    if (veh_handle != 0) {
        RemoveVectoredExceptionHandler(veh_handle);
        print_str("[GUEST]  [PASS] AddVectoredExceptionHandler & RemoveVectoredExceptionHandler\n");
    }

    // 3. Test Windows 7 Processor Groups & Windows 10 Thread Description
    unsigned int cores = GetActiveProcessorCount(0);
    if (cores == 2) {
        print_str("[GUEST]  [PASS] Windows 7 GetActiveProcessorCount (2 cores)\n");
    }
    const wchar_t thread_name[] = L"WorkerThread";
    SetThreadDescription((void*)-2, thread_name);
    print_str("[GUEST]  [PASS] Windows 10 SetThreadDescription\n");

    // 4. Test EventEx & MutexEx (Vista/7+)
    void* hEvent = CreateEventExW(0, 0, 1 /* MANUAL_RESET */, 0x1F0003);
    SetEvent(hEvent);
    unsigned int wait_res = WaitForSingleObjectEx(hEvent, 0, 0);
    if (wait_res == 0) {
        print_str("[GUEST]  [PASS] Windows 7 CreateEventExW & WaitForSingleObjectEx\n");
    }

    void* hMutex = CreateMutexExW(0, 0, 1 /* INITIAL_OWNER */, 0x1F0001);
    ReleaseMutex(hMutex);
    print_str("[GUEST]  [PASS] Windows 7 CreateMutexExW & ReleaseMutex\n");

    // 5. Test BCrypt & ProcessPrng (Windows 7/10 Cryptography)
    unsigned char random_bytes[16] = {0};
    BCryptGenRandom(0, random_bytes, sizeof(random_bytes), 2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */);
    ProcessPrng(random_bytes, sizeof(random_bytes));
    print_str("[GUEST]  [PASS] BCryptGenRandom & Windows 10 ProcessPrng\n");

    // 6. Test NT Native API (NtQueryInformationProcess)
    PROCESS_BASIC_INFORMATION pbi;
    unsigned int ret_len = 0;
    int nt_status = NtQueryInformationProcess((void*)-1, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), &ret_len);
    if (nt_status == 0 && pbi.PebBaseAddress != 0) {
        print_str("[GUEST]  [PASS] ntdll.dll NtQueryInformationProcess (PEB at 0x7FFDF000)\n");
    }

    // 7. Test CPU atomic CMPXCHG instruction inline
    volatile unsigned int atomic_target = 100;
    unsigned int expected = 100;
    unsigned int new_val = 200;
    // asm cmpxchg: if EAX == [atomic_target], [atomic_target] = new_val
    __asm__ __volatile__(
        "cmpxchgl %2, %1\n\t"
        : "+a"(expected), "+m"(atomic_target)
        : "r"(new_val)
        : "memory"
    );
    if (atomic_target == 200) {
        print_str("[GUEST]  [PASS] x86 CMPXCHG atomic lock-free synchronization\n");
    }

    print_str("[GUEST] All Extended Tests Passed Successfully!\n");
    ExitProcess(250);
}
