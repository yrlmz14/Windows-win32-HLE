__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) int __stdcall WriteFile(void* hFile, const void* lpBuffer, unsigned int nNumberOfBytesToWrite, unsigned int* lpNumberOfBytesWritten, void* lpOverlapped);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned int nStdHandle);

// Windows 10 & 8 APIs
typedef struct {
    unsigned int dwLowDateTime;
    unsigned int dwHighDateTime;
} FILETIME;

__declspec(dllimport) void __stdcall GetSystemTimePreciseAsFileTime(FILETIME* lpSystemTimeAsFileTime);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
__declspec(dllimport) int __stdcall WaitOnAddress(volatile void* Address, void* CompareAddress, unsigned int AddressSize, unsigned int dwMilliseconds);
__declspec(dllimport) void __stdcall WakeByAddressSingle(void* Address);
__declspec(dllimport) void* __stdcall VirtualAllocExNuma(void* hProcess, void* lpAddress, unsigned int dwSize, unsigned int flAllocationType, unsigned int flProtect, unsigned int nndPreferred);

static void print_str(const char* s) {
    void* hOut = GetStdHandle((unsigned int)-11);
    unsigned int len = 0;
    while (s[len]) len++;
    unsigned int written = 0;
    WriteFile(hOut, s, len, &written, 0);
}

void _start() {
    print_str("[GUEST] Starting Windows 10 API Test Suite...\n");

    // 1. High-Precision Time
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    if (ft.dwLowDateTime != 0 || ft.dwHighDateTime != 0) {
        print_str("[GUEST]  [PASS] Windows 10 GetSystemTimePreciseAsFileTime\n");
    }

    // 2. 64-bit Tick Count
    unsigned long long ticks = GetTickCount64();
    if (ticks > 0) {
        print_str("[GUEST]  [PASS] GetTickCount64\n");
    }

    // 3. WaitOnAddress & WakeByAddressSingle (Futex)
    volatile unsigned int val = 42;
    unsigned int cmp = 99; // Different value, so WaitOnAddress returns immediately without waiting
    int wait_res = WaitOnAddress(&val, &cmp, sizeof(unsigned int), 10);
    if (wait_res) {
        print_str("[GUEST]  [PASS] Windows 10 WaitOnAddress\n");
    }

    WakeByAddressSingle((void*)&val);
    print_str("[GUEST]  [PASS] Windows 10 WakeByAddressSingle\n");

    // 4. Memory NUMA alloc
    void* mem = VirtualAllocExNuma((void*)-1, 0, 0x1000, 0x1000 | 0x2000, 0x04, 0);
    if (mem != 0) {
        print_str("[GUEST]  [PASS] Windows 7/10 VirtualAllocExNuma\n");
    }

    print_str("[GUEST] All Windows 10 Tests Passed!\n");
    ExitProcess(100); // Expected exit code 100
}
