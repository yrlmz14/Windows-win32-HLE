#ifndef WIN32HLE_TYPES_H
#define WIN32HLE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
typedef uint64_t  QWORD;
typedef int32_t   LONG;
typedef int64_t   LONGLONG;
typedef uint32_t  ULONG;
typedef uint64_t  ULONGLONG;
typedef uint32_t  UINT;
typedef int32_t   INT;
typedef int32_t   BOOL;
typedef uint32_t  DWORD_PTR;
typedef uint32_t  ULONG_PTR;
typedef uint32_t  SIZE_T;
typedef uint32_t  LPVOID;
typedef uint32_t  LPCVOID;
typedef uint32_t  PVOID;
typedef uint32_t  HANDLE;
typedef uint32_t  HMODULE;
typedef uint32_t  HINSTANCE;
typedef uint32_t  FARPROC;
typedef uint16_t  WCHAR;
typedef const char*    LPCSTR;
typedef char*          LPSTR;
typedef const WCHAR*   LPCWSTR;
typedef WCHAR*         LPWSTR;

#define FALSE 0
#define TRUE  1

#define INVALID_HANDLE_VALUE ((HANDLE)0xFFFFFFFF)

typedef struct {
    DWORD LowPart;
    LONG  HighPart;
} LARGE_INTEGER_PARTS;

typedef union {
    struct {
        DWORD LowPart;
        LONG  HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct {
    DWORD dwLength;
    DWORD lpSecurityDescriptor;
    BOOL  bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

// Windows 7 / 10 Synchronization Primitives
typedef struct {
    PVOID Ptr;
} SRWLOCK, *PSRWLOCK;

typedef struct {
    PVOID Ptr;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE;

typedef struct {
    PVOID Ptr;
} INIT_ONCE, *PINIT_ONCE;

// System Information
typedef struct {
    WORD wProcessorArchitecture;
    WORD wReserved;
    DWORD dwPageSize;
    DWORD lpMinimumApplicationAddress;
    DWORD lpMaximumApplicationAddress;
    DWORD dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD  wServicePackMajor;
    WORD  wServicePackMinor;
    WORD  wSuiteMask;
    BYTE  wProductType;
    BYTE  wReserved;
} OSVERSIONINFOEXW, *LPOSVERSIONINFOEXW;

// Memory Status (Windows 2000+) - Essential for 512MB RAM reporting
typedef struct {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

// File Information Classes (Windows Vista / 7+)
typedef enum {
    FileBasicInfo = 0,
    FileStandardInfo = 1,
    FileNameInfo = 2,
    FileRenameInfo = 3,
    FileDispositionInfo = 4,
    FileAllocationInfo = 5,
    FileEndOfFileInfo = 6,
    FileStreamInfo = 7,
    FileCompressionInfo = 8,
    FileAttributeTagInfo = 9,
    FileIdBothDirectoryInfo = 10,
    FileIdBothDirectoryRestartInfo = 11,
    FilePriorityHintInfo = 12,
    FileRemoteProtocolInfo = 13,
    FileFullDirectoryInfo = 14,
    FileFullDirectoryRestartInfo = 15,
    FileStorageInfo = 16,
    FileAlignmentInfo = 17,
    FileIdInfo = 18,
    FileIdExtdDirectoryInfo = 19,
    FileIdExtdDirectoryRestartInfo = 20,
    MaximumFileInfoByHandleClass
} FILE_INFO_BY_HANDLE_CLASS;

typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    DWORD FileAttributes;
} FILE_BASIC_INFO;

typedef struct {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    DWORD NumberOfLinks;
    BOOL DeletePending;
    BOOL Directory;
} FILE_STANDARD_INFO;

typedef struct {
    ULONGLONG VolumeSerialNumber;
    BYTE FileId[16];
} FILE_ID_INFO;

// WIN32_FIND_DATA (Unicode)
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;

// Vectored Exception Handling
typedef struct {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    DWORD ExceptionRecord;
    DWORD ExceptionAddress;
    DWORD NumberParameters;
    DWORD ExceptionInformation[15];
} EXCEPTION_RECORD32;

typedef struct {
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    // FloatSave
    DWORD FloatSave[28];
    DWORD SegGs, SegFs, SegEs, SegDs;
    DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
    DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    BYTE  ExtendedRegisters[512];
} CONTEXT32;

typedef struct {
    DWORD ExceptionRecord; // Ptr to EXCEPTION_RECORD32
    DWORD ContextRecord;   // Ptr to CONTEXT32
} EXCEPTION_POINTERS32;

// LOGICAL_PROCESSOR_INFORMATION_EX (Windows 7+)
typedef enum {
    RelationProcessorCore = 0,
    RelationNumaNode = 1,
    RelationCache = 2,
    RelationProcessorPackage = 3,
    RelationGroup = 4,
    RelationAll = 0xffff
} LOGICAL_PROCESSOR_RELATIONSHIP;

typedef struct {
    BYTE Flags;
    BYTE EfficiencyClass;
    BYTE Reserved[20];
    DWORD GroupCount;
    // Followed by KAFFINITY
} PROCESSOR_RELATIONSHIP;

typedef struct {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    DWORD Size;
    union {
        PROCESSOR_RELATIONSHIP Processor;
        BYTE Reserved[64];
    } u;
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

#endif // WIN32HLE_TYPES_H
