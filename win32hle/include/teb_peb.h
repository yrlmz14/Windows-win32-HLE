#ifndef WIN32HLE_TEB_PEB_H
#define WIN32HLE_TEB_PEB_H

#include "types.h"

#define KUSER_SHARED_DATA_ADDRESS 0x7FFE0000
#define DEFAULT_PEB_ADDRESS       0x7FFDF000
#define DEFAULT_TEB_ADDRESS       0x7FFDE000

typedef enum {
    WIN_VER_XP = 0,
    WIN_VER_7  = 1,
    WIN_VER_10 = 2
} WindowsVersionProfile;

// KUSER_SHARED_DATA at 0x7FFE0000
typedef struct {
    DWORD TickCountLowDeprecated;
    DWORD TickCountMultiplier;
    DWORD InterruptTimeLow;
    DWORD InterruptTimeHigh1;
    DWORD InterruptTimeHigh2;
    DWORD SystemTimeLow;
    DWORD SystemTimeHigh1;
    DWORD SystemTimeHigh2;
    DWORD TimeZoneBiasLow;
    DWORD TimeZoneBiasHigh1;
    DWORD TimeZoneBiasHigh2;
    WORD  ImageNumberLow;
    WORD  ImageNumberHigh;
    WCHAR NtSystemRoot[260];
    DWORD MaxStackTraceDepth;
    DWORD CryptoExponent;
    DWORD TimeZoneId;
    DWORD LargePageMinimum;
    DWORD AitSamplingValue;
    DWORD AppCompatFlag;
    ULONGLONG RNGSeedVersion;
    DWORD GlobalValidationRunlevel;
    LONG  TimeZoneBiasStamp;
    DWORD NtBuildNumber;
    DWORD NtProductType;
    BYTE  ProductTypeIsValid;
    BYTE  Reserved0[1];
    WORD  NativeProcessorArchitecture;
    DWORD NtMajorVersion;
    DWORD NtMinorVersion;
    BYTE  ProcessorFeatures[64];
    DWORD Reserved1;
    DWORD Reserved3;
    DWORD TimeSlip;
    DWORD AlternativeArchitecture;
    DWORD AltArchitecturePad[1];
    LARGE_INTEGER SystemExpirationDate;
    DWORD SuiteMask;
    BYTE  KdDebuggerEnabled;
    BYTE  MitigationPolicies;
    BYTE  Reserved6[2];
    DWORD ActiveConsoleId;
    DWORD DismountCount;
    DWORD ComPlusPackage;
    DWORD LastSystemRITEventTickCount;
    DWORD NumberOfPhysicalPages;
    BYTE  SafeBootMode;
    BYTE  VirtualizationFlags;
    BYTE  Reserved12[2];
    DWORD SharedDataFlags;
    // ...
} KUSER_SHARED_DATA;

// Simplified PEB (Process Environment Block)
typedef struct {
    BYTE  InheritedAddressSpace;
    BYTE  ReadImageFileExecOptions;
    BYTE  BeingDebugged;
    BYTE  BitField;
    DWORD Mutant;
    DWORD ImageBaseAddress;
    DWORD Ldr;
    DWORD ProcessParameters;
    DWORD SubSystemData;
    DWORD ProcessHeap;
    DWORD FastPebLock;
    DWORD AtlThunkSListPtr;
    DWORD IFEOKey;
    DWORD CrossProcessFlags;
    DWORD UserSharedInfoPtr;
    DWORD SystemReserved;
    DWORD AtlThunkSListPtr32;
    DWORD ApiSetMap;                    // Win 7 / 10 ApiSetMap pointer!
    DWORD TlsExpansionCounter;
    DWORD TlsBitmap;
    DWORD TlsBitmapBits[2];
    DWORD ReadOnlySharedMemoryBase;
    DWORD HotpatchInformation;
    DWORD ReadOnlyStaticServerData;
    DWORD AnsiCodePageData;
    DWORD OemCodePageData;
    DWORD UnicodeCaseTableData;
    DWORD NumberOfProcessors;
    DWORD NtGlobalFlag;
    LARGE_INTEGER CriticalSectionTimeout;
    DWORD HeapSegmentReserve;
    DWORD HeapSegmentCommit;
    DWORD HeapDeCommitTotalFreeThreshold;
    DWORD HeapDeCommitFreeBlockThreshold;
    DWORD NumberOfHeaps;
    DWORD MaximumNumberOfHeaps;
    DWORD ProcessHeaps;
    DWORD GdiSharedHandleTable;
    DWORD ProcessStarterHelper;
    DWORD GdiDCAttributeList;
    DWORD LoaderLock;
    DWORD OSMajorVersion;
    DWORD OSMinorVersion;
    WORD  OSBuildNumber;
    WORD  OSCSDVersion;
    DWORD OSPlatformId;
    DWORD ImageSubsystem;
    DWORD ImageSubsystemMajorVersion;
    DWORD ImageSubsystemMinorVersion;
    // ...
} PEB32;

// Simplified TEB (Thread Environment Block)
typedef struct {
    DWORD CurrentExceptionList; // FS:[0x00]
    DWORD StackBase;            // FS:[0x04]
    DWORD StackLimit;           // FS:[0x08]
    DWORD SubSystemTib;         // FS:[0x0C]
    DWORD FiberData;            // FS:[0x10]
    DWORD ArbitraryUserPointer; // FS:[0x14]
    DWORD Self;                 // FS:[0x18] -> Pointer to TEB itself
    DWORD EnvironmentPointer;   // FS:[0x1C]
    DWORD ClientIdUniqueProcess;// FS:[0x20]
    DWORD ClientIdUniqueThread; // FS:[0x24]
    DWORD ActiveRpcHandle;      // FS:[0x28]
    DWORD ThreadLocalStoragePointer; // FS:[0x2C]
    DWORD ProcessEnvironmentBlock;   // FS:[0x30] -> Pointer to PEB
    DWORD LastErrorValue;       // FS:[0x34]
    DWORD CountOfOwnedCriticalSections; // FS:[0x38]
    DWORD CsrClientThread;      // FS:[0x3C]
    DWORD Win32ThreadInfo;      // FS:[0x40]
    DWORD User32Reserved[26];
    DWORD UserReserved[5];
    DWORD WOW32Reserved;
    DWORD CurrentLocale;
    DWORD FpSoftwareStatusRegister;
    DWORD SystemReserved1[54];
    DWORD ExceptionCode;
    DWORD ActivationContextStackPointer;
    // ...
} TEB32;

void init_teb_peb_shared_data(WindowsVersionProfile profile, DWORD image_base);
void update_kuser_shared_data_time(void);

#endif // WIN32HLE_TEB_PEB_H
