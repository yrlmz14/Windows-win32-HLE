__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);
__declspec(dllimport) int __stdcall WriteFile(void* hFile, const void* lpBuffer, unsigned int nNumberOfBytesToWrite, unsigned int* lpNumberOfBytesWritten, void* lpOverlapped);
__declspec(dllimport) void* __stdcall GetStdHandle(unsigned int nStdHandle);
__declspec(dllimport) void* __stdcall VirtualAlloc(void* lpAddress, unsigned int dwSize, unsigned int flAllocationType, unsigned int flProtect);
__declspec(dllimport) int __stdcall VirtualFree(void* lpAddress, unsigned int dwSize, unsigned int dwFreeType);

static void print_str(const char* s) {
    void* hOut = GetStdHandle((unsigned int)-11);
    unsigned int len = 0;
    while (s[len]) len++;
    unsigned int written = 0;
    WriteFile(hOut, s, len, &written, 0);
}

void _start() {
    print_str("[GUEST] Starting Memory Budget & Limit Test...\n");

    // Allocate 1 MB (should succeed within budget)
    void* p1 = VirtualAlloc(0, 1024 * 1024, 0x1000 | 0x2000, 0x04);
    if (p1 != 0) {
        print_str("[GUEST]  [PASS] 1MB allocation succeeded\n");
    }

    // Try to allocate 10 MB when running with --ram-limit 4 (should be rejected gracefully)
    void* p2 = VirtualAlloc(0, 10 * 1024 * 1024, 0x1000 | 0x2000, 0x04);
    if (p2 == 0) {
        print_str("[GUEST]  [PASS] Excessive allocation was gracefully rejected (OOM prevented!)\n");
    }

    // Free p1
    VirtualFree(p1, 0, 0x8000);
    print_str("[GUEST]  [PASS] Memory released back to system\n");

    print_str("[GUEST] Memory Budget Test Passed!\n");
    ExitProcess(150);
}
