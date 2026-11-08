all: test_win7.exe test_win10.exe test_apiset.exe test_memlimit.exe test_extended.exe

kernel32.lib:
	printf "LIBRARY KERNEL32.DLL\nEXPORTS\n  ExitProcess@4\n  GetStdHandle@4\n  WriteFile@20\n  AcquireSRWLockExclusive@4\n  ReleaseSRWLockExclusive@4\n  AcquireSRWLockShared@4\n  ReleaseSRWLockShared@4\n  InitializeSRWLock@4\n  InitOnceInitialize@4\n  InitOnceBeginInitialize@16\n  InitOnceComplete@12\n  GetSystemTimePreciseAsFileTime@4\n  GetTickCount64@0\n  WaitOnAddress@16\n  WakeByAddressSingle@4\n  VirtualAllocExNuma@24\n  GetLargePageMinimum@0\n" > kernel32.def
	llvm-dlltool -m i386 -d kernel32.def -l kernel32.lib

api-ms-win-core-synch-l1-1-0.lib:
	printf "LIBRARY api-ms-win-core-synch-l1-1-0.dll\nEXPORTS\n  AcquireSRWLockExclusive@4\n  ReleaseSRWLockExclusive@4\n  WaitOnAddress@16\n  WakeByAddressSingle@4\n" > synch.def
	llvm-dlltool -m i386 -d synch.def -l api-ms-win-core-synch-l1-1-0.lib

api-ms-win-core-processthreads-l1-1-0.lib:
	printf "LIBRARY api-ms-win-core-processthreads-l1-1-0.dll\nEXPORTS\n  ExitProcess@4\n" > proc.def
	llvm-dlltool -m i386 -d proc.def -l api-ms-win-core-processthreads-l1-1-0.lib

api-ms-win-core-file-l1-1-0.lib:
	printf "LIBRARY api-ms-win-core-file-l1-1-0.dll\nEXPORTS\n  WriteFile@20\n" > file.def
	llvm-dlltool -m i386 -d file.def -l api-ms-win-core-file-l1-1-0.lib

api-ms-win-core-processenvironment-l1-1-0.lib:
	printf "LIBRARY api-ms-win-core-processenvironment-l1-1-0.dll\nEXPORTS\n  GetStdHandle@4\n" > pe.def
	llvm-dlltool -m i386 -d pe.def -l api-ms-win-core-processenvironment-l1-1-0.lib

test_win7.exe: test_win7.c kernel32.lib
	clang -target i686-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,-entry:_start -Wl,-subsystem:console test_win7.c kernel32.lib -o $@

test_win10.exe: test_win10.c kernel32.lib
	clang -target i686-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,-entry:_start -Wl,-subsystem:console test_win10.c kernel32.lib -o $@

# Pure ApiSet binary - zero direct kernel32 imports!
test_apiset.exe: test_apiset.c api-ms-win-core-synch-l1-1-0.lib api-ms-win-core-processthreads-l1-1-0.lib api-ms-win-core-file-l1-1-0.lib api-ms-win-core-processenvironment-l1-1-0.lib
	clang -target i686-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,-entry:_start -Wl,-subsystem:console test_apiset.c api-ms-win-core-synch-l1-1-0.lib api-ms-win-core-processthreads-l1-1-0.lib api-ms-win-core-file-l1-1-0.lib api-ms-win-core-processenvironment-l1-1-0.lib -o $@

test_memlimit.exe: test_memlimit.c kernel32.lib
	clang -target i686-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,-entry:_start -Wl,-subsystem:console test_memlimit.c kernel32.lib -o $@

bcrypt.lib:
	printf "LIBRARY BCRYPT.DLL\nEXPORTS\n  BCryptGenRandom@16\n  ProcessPrng@8\n" > bcrypt.def
	llvm-dlltool -m i386 -d bcrypt.def -l bcrypt.lib

ntdll.lib:
	printf "LIBRARY NTDLL.DLL\nEXPORTS\n  NtQueryInformationProcess@20\n" > ntdll.def
	llvm-dlltool -m i386 -d ntdll.def -l ntdll.lib

test_extended.exe: test_extended.c kernel32.lib bcrypt.lib ntdll.lib
	clang -target i686-pc-windows-msvc -fuse-ld=lld -nostdlib -Wl,-entry:_start -Wl,-subsystem:console test_extended.c kernel32.lib bcrypt.lib ntdll.lib -o $@

clean:
	rm -f *.exe *.lib *.def *.o

.PHONY: all clean
