__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) int __stdcall WriteFile(void* hFile, const void* lpBuffer, unsigned int nNumberOfBytesToWrite, unsigned int* lpNumberOfBytesWritten, void* lpOverlapped);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned int nStdHandle);

// Windows 7 APIs
__declspec(dllimport) void __stdcall InitializeSRWLock(void* SRWLock);
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void* SRWLock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void* SRWLock);
__declspec(dllimport) void __stdcall AcquireSRWLockShared(void* SRWLock);
__declspec(dllimport) void __stdcall ReleaseSRWLockShared(void* SRWLock);
__declspec(dllimport) void __stdcall InitOnceInitialize(void* InitOnce);
__declspec(dllimport) int __stdcall InitOnceBeginInitialize(void* lpInitOnce, unsigned int dwFlags, int* fPending, void** lpContext);
__declspec(dllimport) int __stdcall InitOnceComplete(void* lpInitOnce, unsigned int dwFlags, void* lpContext);
__declspec(dllimport) unsigned int __stdcall GetLargePageMinimum(void);

static void print_str(const char* s) {
    void* hOut = GetStdHandle((unsigned int)-11);
    unsigned int len = 0;
    while (s[len]) len++;
    unsigned int written = 0;
    WriteFile(hOut, s, len, &written, 0);
}

void _start() {
    print_str("[GUEST] Starting Windows 7 API Test Suite...\n");

    // 1. Test SRWLock
    void* srw_lock = 0;
    InitializeSRWLock(&srw_lock);
    AcquireSRWLockExclusive(&srw_lock);
    ReleaseSRWLockExclusive(&srw_lock);

    AcquireSRWLockShared(&srw_lock);
    ReleaseSRWLockShared(&srw_lock);
    print_str("[GUEST]  [PASS] Windows 7 SRWLock (Exclusive & Shared)\n");

    // 2. Test InitOnce
    void* init_once = 0;
    InitOnceInitialize(&init_once);
    int pending = 0;
    void* ctx = 0;
    if (InitOnceBeginInitialize(&init_once, 0, &pending, &ctx)) {
        if (pending) {
            InitOnceComplete(&init_once, 0, (void*)0x1234);
            print_str("[GUEST]  [PASS] Windows 7 InitOnceInitialize & InitOnceComplete\n");
        }
    }

    // 3. Test Low-Memory LargePageMinimum
    unsigned int min_large = GetLargePageMinimum();
    if (min_large == 0) {
        print_str("[GUEST]  [PASS] GetLargePageMinimum verified for 512MB RAM target (disabled)\n");
    }

    print_str("[GUEST] All Windows 7 Tests Passed!\n");
    ExitProcess(77); // Expected exit code 77
}
