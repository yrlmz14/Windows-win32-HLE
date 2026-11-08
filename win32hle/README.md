# Win32 HLE (Windows 7 & Windows 10 Target)

A high-performance, low-memory Win32 High-Level Emulation (HLE) engine engineered specifically to run **Windows 7 and Windows 10** binaries on memory-constrained hardware (e.g. 512 MB RAM embedded boards, handhelds, or microVMs).

---

## 1. Why XP-Era HLEs Fail on Windows 7 & 10 Binaries

Binaries built for modern Windows (MSVC 2015–2022, modern MinGW, Clang) fail immediately on XP-era Win32 HLEs due to three fundamental architectural changes:

1. **ApiSet Schema Redirection (`api-ms-win-*`, `ext-ms-*`)**:
   Modern Windows splits `kernel32.dll`, `user32.dll`, and the CRT into virtual API sets (e.g., `api-ms-win-core-synch-l1-2-0.dll`, `api-ms-win-crt-runtime-l1-1-0.dll`). If an HLE only maps monolithic DLL names, these binaries will fail at load time with "DLL not found".
2. **Missing Vista / Win7 / Win10 Synchronization Primitives**:
   Modern compilers and runtime libraries (`std::mutex`, `std::condition_variable`, `std::call_once`, `std::atomic::wait`) avoid heavy Win32 kernel handles (`CreateMutex`, `CreateEvent`) and instead require:
   - **SRW Locks (Slim Reader/Writer)**: `AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`, `AcquireSRWLockShared`, etc.
   - **Condition Variables**: `SleepConditionVariableSRW`, `WakeConditionVariable`, `WakeAllConditionVariable`.
   - **One-Time Initialization**: `InitOnceInitialize`, `InitOnceBeginInitialize`, `InitOnceComplete`.
   - **WaitOnAddress (Futex)**: `WaitOnAddress`, `WakeByAddressSingle`, `WakeByAddressAll` (Windows 8/10).
3. **KUSER_SHARED_DATA & TEB/PEB Expectations**:
   Modern binaries query `0x7FFE0000` (`KUSER_SHARED_DATA`) directly for `NtMajorVersion`, `NtMinorVersion`, `NtBuildNumber`, `ProcessorFeatures`, and precise time without issuing syscalls.

---

## 2. ARM64 (AArch64) Binary Emulator & 512 MB RAM Optimization

Running x86 Win32 binaries on an **ARM64 host** with only **512 MB RAM** requires an ultra-lightweight dynamic binary recompiler (Dynarec):

- **Direct x86-to-ARM64 Register Mapping**:
  Pinned to callee-saved ARM64 32-bit registers to avoid register-allocation overhead:
  | Guest x86 | Host ARM64 | Role |
  |-----------|------------|------|
  | `EAX`     | `W19`      | Accumulator |
  | `ECX`     | `W20`      | Counter |
  | `EDX`     | `W21`      | Data |
  | `EBX`     | `W22`      | Base |
  | `ESP`     | `W23`      | Guest Stack Pointer |
  | `EBP`     | `W24`      | Frame Pointer |
  | `ESI`     | `W25`      | Source Index |
  | `EDI`     | `W26`      | Destination Index |
  | `FS:0`    | `X27`      | Guest TEB Pointer |
  | `Arena`   | `X28`      | 2GB Virtual Arena Base |

- **Zero-Overhead 32-bit Memory Offsetting via ARM64 `UXTW`**:
  ARM64 hardware natively supports 32-bit zero-extended offset addressing:
  `LDR Wt, [X28, Wreg, UXTW]` / `STR Wt, [X28, Wreg, UXTW]`.
  This translates Win32 guest memory accesses directly into a single native A64 machine cycle without needing additional masking or bitshift instructions!
- **Lean 4 MB JIT Code Cache**:
  Allocates a circular 4 MB executable cache buffer (`PROT_READ | PROT_WRITE | PROT_EXEC`). When the cache fills up, it cleanly flushes and rewrites hot blocks, preserving almost the entire 512 MB RAM for the application.
- **Direct Synthetic HLE Trap Calling**:
  When a block hits the synthetic HLE trap `0x0F 0x3F <id>`, the dynarec spills the registers, calls the C HLE dispatch handler via `BLR X8`, captures `EAX` from `W0`, and returns seamlessly.
- **Dual Engine (Dynarec + Interpreter Fallback)**:
  Uses the ARM64 dynarec for hot instruction sequences, falling back to the portable interpreter for complex control-flow transfers. Runs identically on ARM64 targets and x86_64 development environments.
- **512 MB Physical RAM Commit Ceiling (`--ram-limit <MB>`)**:
  Default limit is 384 MB (leaving 128 MB for host OS and buffers). If `VirtualAlloc` requests exceed the commit budget, the call fails cleanly with `ERROR_NOT_ENOUGH_MEMORY` (code 8) rather than triggering the host Linux Out-Of-Memory (OOM) killer.
- **Immediate Physical Return (`MADV_DONTNEED`)**:
  `VirtualFree` with `MEM_DECOMMIT` or `MEM_RELEASE` executes `madvise(..., MADV_DONTNEED)` to immediately return physical pages to the host kernel.

---

## 3. Directory Structure

```
win32hle/
├── include/
│   ├── types.h               # Win32 & Win7/10 data types and structures
│   ├── pe.h                  # PE32 format headers & data directories
│   ├── pe_loader.h           # PE loader interface
│   ├── memory.h              # 2GB virtual memory manager & 512MB RAM budget tracker
│   ├── teb_peb.h             # TEB, PEB, and KUSER_SHARED_DATA (0x7FFE0000)
│   ├── apiset.h              # Windows 7/10 ApiSet resolution engine
│   ├── sync.h                # SRWLock, ConditionVariable, InitOnce, WaitOnAddress
│   ├── hle.h                 # HLE dispatch table & handle manager
│   ├── cpu.h                 # Fast x86 execution engine with HLE escape hook
│   └── arm64_dynarec.h       # ARM64 binary dynarec, JIT cache & A64 emitters
├── src/
│   ├── main.c                # CLI entry point (--winver, --ram-limit, --jit)
│   ├── memory.c              # VirtualAlloc/VirtualFree/mprotect implementation
│   ├── teb_peb.c             # Initializes TEB/PEB and KUSER_SHARED_DATA
│   ├── apiset.c              # Maps api-ms-win-* and ext-ms-* to backing DLLs
│   ├── hle_dispatch.c        # HLE dispatch table, stdcall/cdecl caller, handles
│   ├── kernel32_win7_10.c    # Windows 7 & Windows 10 modern APIs
│   ├── kernel32_base.c       # Classic Win32 APIs (Console, Files, Processes, Heaps)
│   ├── ucrt_thunks.c         # Universal CRT (UCRT) thunks
│   ├── sync.c                # SRWLock, ConditionVariable, WaitOnAddress implementation
│   ├── pe_loader.c           # PE parser, section mapper, relocations, IAT hooker
│   ├── cpu.c                 # Instruction interpreter with HLE escape trap
│   └── arm64_dynarec.c       # x86-to-ARM64 JIT compiler & native block runner
├── tests/
│   ├── Makefile              # Builds test Windows 7/10 PE32 executables
│   ├── test_win7.c           # Test program for SRWLock, InitOnce, LargePageMinimum
│   ├── test_win10.c          # Test program for GetSystemTimePreciseAsFileTime, WaitOnAddress, NUMA
│   ├── test_apiset.c         # Pure ApiSet binary (no direct kernel32 imports)
│   ├── test_memlimit.c       # Tests 512MB physical RAM limit & OOM prevention
│   └── test_extended.c       # MemoryStatusEx, VEH, Processor Groups, BCrypt, NT Native, CMPXCHG
└── Makefile                  # Builds win32hle target
```

---

## 4. Implemented Windows 7 & 10 APIs / Syscalls

### Synchronization (Vista / Windows 7 / 8 / 10)
- `InitializeSRWLock`
- `AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive`
- `AcquireSRWLockShared` / `ReleaseSRWLockShared`
- `TryAcquireSRWLockExclusive` / `TryAcquireSRWLockShared`
- `InitializeConditionVariable`
- `SleepConditionVariableSRW`
- `WakeConditionVariable` / `WakeAllConditionVariable`
- `InitOnceInitialize`
- `InitOnceBeginInitialize`
- `InitOnceComplete`
- `WaitOnAddress` (Windows 8 / 10 futex)
- `WakeByAddressSingle` / `WakeByAddressAll` (Windows 8 / 10 futex)
- `CreateEventExW` / `SetEvent` / `ResetEvent`
- `CreateMutexExW` / `ReleaseMutex`
- `CreateSemaphoreExW` / `ReleaseSemaphore`
- `WaitForSingleObjectEx` / `WaitForMultipleObjectsEx`

### High-Precision Timing (Windows 8 / 10)
- `GetSystemTimePreciseAsFileTime` (sub-microsecond precision for `std::chrono`)
- `GetTickCount64` (64-bit millisecond counter)
- `QueryPerformanceCounter` & `QueryPerformanceFrequency`

### System, Processor Groups & Threads
- `GlobalMemoryStatusEx` (Accurately reports 512 MB physical hardware RAM)
- `GetActiveProcessorCount` / `GetMaximumProcessorCount` (Windows 7 Processor Groups)
- `GetActiveProcessorGroupCount`
- `SetThreadDescription` / `GetThreadDescription` (Windows 10 1607+ named thread API)
- `GetThreadId` / `GetProcessId` / `SwitchToThread`
- `GetLogicalProcessorInformationEx`
- `GetProductInfo`
- `KUSER_SHARED_DATA` at `0x7FFE0000` (reports Windows 7 Build 7601 or Windows 10 Build 19045, 512 MB physical page count, processor feature flags)

### Cryptography (Windows 7 / 10)
- `BCryptGenRandom` (`bcrypt.dll` CSPRNG used by modern runtimes)
- `BCryptOpenAlgorithmProvider` / `BCryptCloseAlgorithmProvider`
- `ProcessPrng` (`bcryptprimitives.dll` Windows 10 PRNG)

### Vectored Exception Handling (VEH)
- `AddVectoredExceptionHandler` / `RemoveVectoredExceptionHandler`
- `SetUnhandledExceptionFilter` / `UnhandledExceptionFilter`

### Native NT APIs (`ntdll.dll`)
- `NtQueryInformationProcess` (e.g., `ProcessBasicInformation`, PEB retrieval)
- `NtQuerySystemInformation`
- `RtlCaptureContext`

### Filesystem & Extended File APIs (Windows Vista / 7 / 8 / 10)
- `GetFileInformationByHandleEx` (`FileBasicInfo`, `FileStandardInfo`, `FileIdInfo`)
- `SetFilePointerEx` (64-bit offsets)
- `GetFinalPathNameByHandleW`
- `CreateFile2` (Windows 8 / 10)
- `CreateFileW` / `CreateFileA` / `DeleteFileW`
- `FindFirstFileExW` / `FindNextFileW` / `FindClose`
- `GetFileAttributesW` / `GetCurrentDirectoryW`

### Memory Virtualization (512 MB RAM Aware)
- `VirtualAllocExNuma` (Windows 7+)
- `GetLargePageMinimum` (returns 0 to disable large page RAM waste)
- `PrefetchVirtualMemory` (Windows 8 / 10)
- `DiscardVirtualMemory` (Windows 8.1 / 10 - releases physical pages via `MADV_DONTNEED`)

### Internationalization & NLS
- `GetUserDefaultLocaleName` / `ResolveLocaleName`
- `GetLocaleInfoEx`
- `IdnToAscii` / `IdnToUnicode`

### Thread Pool APIs (Windows 7+)
- `CreateThreadpoolWork` / `SubmitThreadpoolWork` / `CloseThreadpoolWork`
- `WaitForThreadpoolWorkCallbacks`

---

## 5. Build and Run

### Building the HLE Runtime
```bash
cd win32hle
make
```

### Running Applications
```bash
# Run with Windows 10 profile (default) and 384MB commit limit:
./win32hle /path/to/app.exe

# Run with Windows 7 profile and API call tracing:
./win32hle --winver 7 --trace /path/to/app.exe

# Run with a custom memory ceiling (e.g. 256MB):
./win32hle --ram-limit 256 /path/to/app.exe
```

### Running Test Suites
```bash
cd tests
make

# Test Windows 7 APIs:
../win32hle test_win7.exe

# Test Windows 10 APIs:
../win32hle test_win10.exe

# Test pure ApiSet redirection (no kernel32.dll directly in import table):
../win32hle --trace test_apiset.exe

# Test 512MB RAM constraint enforcement & graceful rejection:
../win32hle --ram-limit 4 test_memlimit.exe
```

### Real-World Binary Verification
The runtime has been verified against standard production Win32 PE binaries (such as 32-bit GNU Make 4.4.1 from official `w64devkit`):

```bash
# Print GNU Make version:
./win32hle /tmp/realbin/make.exe --version

# Print GNU Make help menu:
./win32hle /tmp/realbin/make.exe --help

# Run with ARM64 Dynarec active:
./win32hle --jit /tmp/realbin/make.exe --version
```
Example Output:
```
GNU Make 4.4.1
Built for i686-w64-mingw32
Copyright (C) 1988-2023 Free Software Foundation, Inc.
License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.

[HLE] Execution finished. Exit Code: 0 (0x0)
[HLE] Instructions executed: 29094
[HLE] Peak committed memory: 1552 KB / 393216 KB budget
```

