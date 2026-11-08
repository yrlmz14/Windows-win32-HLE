// This test binary intentionally imports from modern Windows 7 / 10 ApiSet DLLs!
__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) int __stdcall WriteFile(void* hFile, const void* lpBuffer, unsigned int nNumberOfBytesToWrite, unsigned int* lpNumberOfBytesWritten, void* lpOverlapped);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned int nStdHandle);

// Imported from api-ms-win-core-synch-l1-1-0.dll
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void* SRWLock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void* SRWLock);
__declspec(dllimport) int __stdcall WaitOnAddress(volatile void* Address, void* CompareAddress, unsigned int AddressSize, unsigned int dwMilliseconds);
__declspec(dllimport) void __stdcall WakeByAddressSingle(void* Address);

static void print_str(const char* s) {
    void* hOut = GetStdHandle((unsigned int)-11);
    unsigned int len = 0;
    while (s[len]) len++;
    unsigned int written = 0;
    WriteFile(hOut, s, len, &written, 0);
}

void _start() {
    print_str("[GUEST] Starting ApiSet Schema Redirection Test...\n");
    print_str("[GUEST]  Importing from 'api-ms-win-core-synch-l1-1-0.dll' and 'api-ms-win-core-processthreads-l1-1-0.dll'\n");

    void* lock = 0;
    AcquireSRWLockExclusive(&lock);
    ReleaseSRWLockExclusive(&lock);
    print_str("[GUEST]  [PASS] Successfully invoked SRWLock via ApiSet redirection!\n");

    volatile unsigned int val = 1;
    unsigned int cmp = 2;
    WaitOnAddress(&val, &cmp, sizeof(unsigned int), 5);
    WakeByAddressSingle((void*)&val);
    print_str("[GUEST]  [PASS] Successfully invoked WaitOnAddress via ApiSet redirection!\n");

    print_str("[GUEST] ApiSet Redirection Suite Completed Successfully!\n");
    ExitProcess(200);
}
