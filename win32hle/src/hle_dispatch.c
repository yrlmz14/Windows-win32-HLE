#include "hle.h"
#include "apiset.h"
#include "memory.h"
#include "teb_peb.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

HLEContext g_hle = {0};

#define MAX_HANDLES 1024

typedef struct {
    void* native_ptr;
    uint32_t type;
    bool in_use;
} HandleEntry;

static HandleEntry s_handles[MAX_HANDLES] = {0};

void hle_init(bool trace_calls) {
    memset(&g_hle, 0, sizeof(HLEContext));
    g_hle.trace_calls = trace_calls;

    // Allocate executable memory page for thunks
    mem_virtual_alloc(THUNK_BASE_ADDRESS, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    // Initialize standard console handles
    // STD_INPUT_HANDLE  ((DWORD)-10) -> 0xFFFFFFF6
    // STD_OUTPUT_HANDLE ((DWORD)-11) -> 0xFFFFFFF5
    // STD_ERROR_HANDLE  ((DWORD)-12) -> 0xFFFFFFF4
    s_handles[1].native_ptr = (void*)(uintptr_t)0; // stdin fd 0
    s_handles[1].type = HANDLE_TYPE_FILE;
    s_handles[1].in_use = true;

    s_handles[2].native_ptr = (void*)(uintptr_t)1; // stdout fd 1
    s_handles[2].type = HANDLE_TYPE_FILE;
    s_handles[2].in_use = true;

    s_handles[3].native_ptr = (void*)(uintptr_t)2; // stderr fd 2
    s_handles[3].type = HANDLE_TYPE_FILE;
    s_handles[3].in_use = true;
}

int hle_register(const char* module, const char* name, uint32_t ordinal,
                 HLEHandler handler, uint8_t num_args, bool is_cdecl) {
    if (g_hle.count >= MAX_HLE_FUNCTIONS) {
        fprintf(stderr, "HLE function table full!\n");
        return -1;
    }

    uint32_t id = g_hle.count++;
    HLEFunction* fn = &g_hle.functions[id];
    fn->module_name = module;
    fn->function_name = name;
    fn->ordinal = ordinal;
    fn->handler = handler;
    fn->num_args = num_args;
    fn->is_cdecl = is_cdecl;
    fn->thunk_address = THUNK_BASE_ADDRESS + (id * THUNK_STRIDE);

    // Write x86 thunk machine code:
    // 0x0F 0x3F <id_low> <id_high> (4-byte trap for our CPU)
    // if stdcall: 0xC2 <arg_bytes_low> <arg_bytes_high> (ret N)
    // if cdecl:   0xC3 (ret)
    uint8_t thunk_code[8] = {
        0x0F, 0x3F, (uint8_t)(id & 0xFF), (uint8_t)((id >> 8) & 0xFF),
        0x90, 0x90, 0x90, 0x90
    };

    if (is_cdecl || num_args == 0) {
        thunk_code[4] = 0xC3; // ret
    } else {
        uint16_t bytes = num_args * 4;
        thunk_code[4] = 0xC2; // ret N
        thunk_code[5] = (uint8_t)(bytes & 0xFF);
        thunk_code[6] = (uint8_t)((bytes >> 8) & 0xFF);
    }

    mem_write(fn->thunk_address, thunk_code, sizeof(thunk_code));
    return id;
}

uint32_t hle_resolve(const char* module, const char* name, uint32_t ordinal) {
    const char* resolved_module = apiset_resolve(module);

    if (name) {
        if (strcasecmp(name, "_iob") == 0) {
            extern uint32_t msvcrt_get_iob_addr(void);
            return msvcrt_get_iob_addr();
        }
        if (strcasecmp(name, "__lc_codepage") == 0) {
            static uint32_t s_codepage_box = 0;
            if (!s_codepage_box) {
                s_codepage_box = mem_virtual_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                mem_write_u32(s_codepage_box, 1252);
            }
            return s_codepage_box;
        }
    }

    for (uint32_t i = 0; i < g_hle.count; i++) {
        HLEFunction* fn = &g_hle.functions[i];

        // Check module match (case insensitive, strip .dll if needed)
        bool mod_match = (strcasecmp(fn->module_name, resolved_module) == 0);
        if (!mod_match) {
            // Also match if both refer to kernel32/ntdll/etc.
            const char* fn_res = apiset_resolve(fn->module_name);
            if (strcasecmp(fn_res, resolved_module) == 0) {
                mod_match = true;
            }
        }
        if (!mod_match) continue;

        if (name && fn->function_name) {
            if (strcasecmp(fn->function_name, name) == 0) {
                return fn->thunk_address;
            }
            // Strip stdcall name decoration: "_Func@N" or "Func@N"
            const char* base_name = (name[0] == '_') ? name + 1 : name;
            const char* at = strchr(base_name, '@');
            if (at) {
                size_t unadorned_len = at - base_name;
                if (strncasecmp(fn->function_name, base_name, unadorned_len) == 0 &&
                    fn->function_name[unadorned_len] == '\0') {
                    return fn->thunk_address;
                }
            }
        } else if (ordinal && fn->ordinal == ordinal) {
            return fn->thunk_address;
        }
    }

    // Pass 2: Fallback across any registered module for named functions
    if (name) {
        for (uint32_t i = 0; i < g_hle.count; i++) {
            HLEFunction* fn = &g_hle.functions[i];
            if (!fn->function_name) continue;
            if (strcasecmp(fn->function_name, name) == 0) {
                return fn->thunk_address;
            }
            const char* base_name = (name[0] == '_') ? name + 1 : name;
            const char* at = strchr(base_name, '@');
            if (at) {
                size_t unadorned_len = at - base_name;
                if (strncasecmp(fn->function_name, base_name, unadorned_len) == 0 &&
                    fn->function_name[unadorned_len] == '\0') {
                    return fn->thunk_address;
                }
            }
        }
    }

    return 0; // Not found
}

static uint32_t hle_generic_stub_handler(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // Default safe return value for unimplemented APIs
}

typedef struct {
    const char* name;
    uint8_t num_args;
    bool is_cdecl;
} ApiSignature;

static const ApiSignature s_known_signatures[] = {
    // KERNEL32
    { "ResolveDelayLoadedAPI", 6, false },
    { "DelayLoadFailureHook", 2, false },
    { "GetProcAddress", 2, false },
    { "CreateMutexExW", 4, false },
    { "AcquireSRWLockShared", 1, false },
    { "ReleaseSRWLockShared", 1, false },
    { "AcquireSRWLockExclusive", 1, false },
    { "ReleaseSRWLockExclusive", 1, false },
    { "DeleteCriticalSection", 1, false },
    { "EnterCriticalSection", 1, false },
    { "LeaveCriticalSection", 1, false },
    { "InitializeCriticalSectionEx", 3, false },
    { "InitializeCriticalSectionAndSpinCount", 2, false },
    { "GetCurrentProcessId", 0, false },
    { "GetCurrentThreadId", 0, false },
    { "GetCurrentProcess", 0, false },
    { "GetProcessHeap", 0, false },
    { "GetModuleHandleW", 1, false },
    { "GetModuleHandleExW", 3, false },
    { "GetModuleFileNameW", 3, false },
    { "GetModuleFileNameA", 3, false },
    { "DebugBreak", 0, false },
    { "IsDebuggerPresent", 0, false },
    { "GlobalFree", 1, false },
    { "GlobalAlloc", 2, false },
    { "GlobalLock", 1, false },
    { "GlobalUnlock", 1, false },
    { "GetLocaleInfoW", 4, false },
    { "CreateFileW", 7, false },
    { "ReadFile", 5, false },
    { "WriteFile", 5, false },
    { "MulDiv", 3, false },
    { "GetCommandLineW", 0, false },
    { "HeapSetInformation", 4, false },
    { "FreeLibrary", 1, false },
    { "FindFirstFileW", 2, false },
    { "FindClose", 1, false },
    { "CompareStringOrdinal", 5, false },
    { "LocalAlloc", 2, false },
    { "LocalFree", 1, false },
    { "LocalLock", 1, false },
    { "LocalUnlock", 1, false },
    { "LocalReAlloc", 3, false },
    { "LocalSize", 1, false },
    { "FoldStringW", 5, false },
    { "GetUserDefaultUILanguage", 0, false },
    { "GetLocalTime", 1, false },
    { "GetDateFormatW", 6, false },
    { "GetTimeFormatW", 6, false },
    { "WideCharToMultiByte", 8, false },
    { "MultiByteToWideChar", 6, false },
    { "GetFileAttributesW", 1, false },
    { "GetFileAttributesExW", 3, false },
    { "GetFileInformationByHandle", 2, false },
    { "GetFullPathNameW", 4, false },
    { "GetACP", 0, false },
    { "DeleteFileW", 1, false },
    { "SetEndOfFile", 1, false },
    { "CreateFileMappingW", 6, false },
    { "MapViewOfFile", 5, false },
    { "UnmapViewOfFile", 1, false },
    { "GetStartupInfoW", 1, false },
    { "lstrcmpiW", 2, false },
    { "FindNLSString", 7, false },
    { "GetDiskFreeSpaceExW", 4, false },
    { "CreateDirectoryW", 2, false },
    { "RegisterApplicationRestart", 2, false },
    { "CreateSemaphoreExW", 5, false },
    { "CreateThreadpoolTimer", 3, false },
    { "SetThreadpoolTimer", 4, false },
    { "WaitForThreadpoolTimerCallbacks", 2, false },
    { "CloseThreadpoolTimer", 1, false },
    { "CloseHandle", 1, false },
    { "OpenSemaphoreW", 3, false },
    { "WaitForSingleObject", 2, false },
    { "WaitForSingleObjectEx", 3, false },
    { "ReleaseMutex", 1, false },
    { "ReleaseSemaphore", 3, false },
    { "OutputDebugStringW", 1, false },
    { "GetLastError", 0, false },
    { "SetLastError", 1, false },
    { "FormatMessageW", 7, false },
    { "HeapAlloc", 3, false },
    { "HeapFree", 3, false },
    { "RaiseException", 4, false },
    { "SetUnhandledExceptionFilter", 1, false },
    { "UnhandledExceptionFilter", 1, false },
    { "TerminateProcess", 2, false },
    { "IsProcessorFeaturePresent", 1, false },
    { "GetProcessMitigationPolicy", 4, false },
    { "CreateEventExW", 4, false },
    { "CreateEventW", 4, false },
    { "ResetEvent", 1, false },
    { "SetEvent", 1, false },
    { "QueryPerformanceCounter", 1, false },
    { "GetTickCount", 0, false },
    { "GetSystemTimeAsFileTime", 1, false },
    { "InitializeSListHead", 1, false },
    { "LoadLibraryExW", 3, false },
    { "Sleep", 1, false },

    // USER32
    { "GetFocus", 0, false },
    { "PostMessageW", 4, false },
    { "GetMenu", 1, false },
    { "CheckMenuItem", 3, false },
    { "GetSubMenu", 2, false },
    { "EnableMenuItem", 3, false },
    { "ShowWindow", 2, false },
    { "GetDC", 1, false },
    { "ReleaseDC", 2, false },
    { "SetCursor", 1, false },
    { "GetDpiForWindow", 1, false },
    { "SetActiveWindow", 1, false },
    { "LoadStringW", 4, false },
    { "DefWindowProcW", 4, false },
    { "IsIconic", 1, false },
    { "SetFocus", 1, false },
    { "PostQuitMessage", 1, false },
    { "DestroyWindow", 1, false },
    { "MessageBeep", 1, false },
    { "GetForegroundWindow", 0, false },
    { "GetDlgCtrlID", 1, false },
    { "SetWindowPos", 7, false },
    { "RedrawWindow", 4, false },
    { "GetKeyboardLayout", 1, false },
    { "CharNextW", 1, false },
    { "SetWinEventHook", 7, false },
    { "GetMessageW", 4, false },
    { "TranslateAcceleratorW", 3, false },
    { "IsDialogMessageW", 2, false },
    { "TranslateMessage", 1, false },
    { "DispatchMessageW", 1, false },
    { "UnhookWinEvent", 1, false },
    { "SetWindowTextW", 2, false },
    { "OpenClipboard", 1, false },
    { "IsClipboardFormatAvailable", 1, false },
    { "CloseClipboard", 0, false },
    { "SetDlgItemTextW", 3, false },
    { "GetDlgItemTextW", 4, false },
    { "EndDialog", 2, false },
    { "SendDlgItemMessageW", 5, false },
    { "SetScrollPos", 4, false },
    { "InvalidateRect", 3, false },
    { "UpdateWindow", 1, false },
    { "GetWindowPlacement", 2, false },
    { "SetWindowPlacement", 2, false },
    { "CharUpperW", 1, false },
    { "GetSystemMenu", 2, false },
    { "LoadAcceleratorsW", 2, false },
    { "SetWindowLongW", 3, false },
    { "CreateWindowExW", 12, false },
    { "MonitorFromWindow", 2, false },
    { "RegisterWindowMessageW", 1, false },
    { "LoadCursorW", 2, false },
    { "RegisterClassExW", 1, false },
    { "GetWindowTextLengthW", 1, false },
    { "GetWindowLongW", 2, false },
    { "PeekMessageW", 5, false },
    { "GetWindowTextW", 3, false },
    { "EnableWindow", 2, false },
    { "CreateDialogParamW", 5, false },
    { "DrawTextExW", 6, false },
    { "LoadIconW", 2, false },
    { "LoadImageW", 6, false },
    { "DialogBoxParamW", 5, false },
    { "SetThreadDpiAwarenessContext", 1, false },
    { "SendMessageW", 4, false },
    { "MoveWindow", 6, false },
    { "GetClientRect", 2, false },
    { "MessageBoxW", 4, false },

    // GDI32
    { "CreateDCW", 4, false },
    { "StartPage", 1, false },
    { "StartDocW", 2, false },
    { "SetAbortProc", 2, false },
    { "DeleteDC", 1, false },
    { "EndDoc", 1, false },
    { "AbortDoc", 1, false },
    { "EndPage", 1, false },
    { "GetTextMetricsW", 2, false },
    { "SetBkMode", 2, false },
    { "LPtoDP", 3, false },
    { "SetWindowExtEx", 4, false },
    { "SetViewportExtEx", 4, false },
    { "SetMapMode", 2, false },
    { "GetTextExtentPoint32W", 4, false },
    { "TextOutW", 5, false },
    { "EnumFontsW", 4, false },
    { "GetTextFaceW", 3, false },
    { "SelectObject", 2, false },
    { "DeleteObject", 1, false },
    { "CreateFontIndirectW", 1, false },
    { "GetDeviceCaps", 2, false },

    // COM / WinRT
    { "CoCreateFreeThreadedMarshaler", 2, false },
    { "CoWaitForMultipleHandles", 5, false },
    { "PropVariantClear", 1, false },
    { "CoTaskMemFree", 1, false },
    { "CoTaskMemAlloc", 1, false },
    { "CoCreateInstance", 5, false },
    { "CoInitializeEx", 2, false },
    { "CoCreateGuid", 1, false },
    { "CoUninitialize", 0, false },
    { "PathFindExtensionW", 1, false },
    { "PathFileExistsW", 1, false },
    { "PathIsFileSpecW", 1, false },
    { "SHStrDupW", 2, false },
    { "GetDpiForMonitor", 4, false },
    { "WindowsDeleteString", 1, false },
    { "WindowsCreateStringReference", 4, false },
    { "WindowsGetStringRawBuffer", 2, false },
    { "WindowsCreateString", 3, false },
    { "SetRestrictedErrorInfo", 1, false },
    { "RoInitialize", 1, false },
    { "RoGetActivationFactory", 3, false },
    { "RoUninitialize", 0, false },
    { "RoGetMatchingRestrictedErrorInfo", 2, false },
    { "EventProviderEnabled", 5, false },
    { "CreateStatusWindowW", 4, false },

    // ADVAPI32
    { "OpenProcessToken", 3, false },
    { "IsTextUnicode", 3, false },
    { "GetTokenInformation", 5, false },
    { "DuplicateEncryptionInfoFile", 5, false },
    { "RegSetValueExW", 6, false },
    { "RegQueryValueExW", 6, false },
    { "RegCreateKeyW", 3, false },
    { "RegCloseKey", 1, false },
    { "RegOpenKeyExW", 5, false },
    { "RegCreateKeyExW", 9, false },
    { "RegDeleteKeyExW", 4, false },
    { "RegQueryInfoKeyW", 12, false },
    { "RegEnumValueW", 8, false },
    { "EventSetInformation", 5, false },
    { "EventRegister", 4, false },
    { "EventUnregister", 2, false },
    { "EventWriteTransfer", 7, false },
    { "DecryptFileW", 2, false },

    // COMDLG32
    { "CommDlgExtendedError", 0, false },
    { "PrintDlgExW", 1, false },
    { "GetOpenFileNameW", 1, false },
    { "GetSaveFileNameW", 1, false },
    { "ReplaceTextW", 1, false },
    { "FindTextW", 1, false },
    { "ChooseFontW", 1, false },
    { "PageSetupDlgW", 1, false },
    { "GetFileTitleW", 3, false },

    // PROPSYS
    { "PropVariantToStringVectorAlloc", 2, false },
    { "PSGetPropertyDescriptionListFromString", 3, false },

    // SHELL32
    { "SHCreateItemFromParsingName", 4, false },
    { "SHGetKnownFolderPath", 4, false },
    { "ShellExecuteW", 6, false },
    { "ShellAboutW", 4, false },
    { "DragQueryFileW", 4, false },
    { "DragFinish", 1, false },
    { "SHAddToRecentDocs", 2, false },
    { "DragAcceptFiles", 2, false },

    // WINSPOOL.DRV
    { "ClosePrinter", 1, false },
    { "GetPrinterDriverW", 6, false },
    { "OpenPrinterW", 3, false },

    // urlmon
    { "FindMimeFromData", 8, false },

    { NULL, 0, false }
};

uint32_t hle_register_stub(const char* module, const char* name, uint32_t ordinal) {
    uint8_t num_args = 0;
    bool is_cdecl = true;

    if (name) {
        const char* at = strchr(name, '@');
        if (at) {
            num_args = (uint8_t)(atoi(at + 1) / 4);
            is_cdecl = false;
        } else {
            // Check known signatures table
            for (int i = 0; s_known_signatures[i].name != NULL; i++) {
                if (strcmp(s_known_signatures[i].name, name) == 0) {
                    num_args = s_known_signatures[i].num_args;
                    is_cdecl = s_known_signatures[i].is_cdecl;
                    break;
                }
            }
        }
    }

    const char* mod_copy = strdup(module ? module : "unknown.dll");
    const char* name_copy = name ? strdup(name) : NULL;
    int id = hle_register(mod_copy, name_copy, ordinal, hle_generic_stub_handler, num_args, is_cdecl);
    if (id >= 0) {
        return g_hle.functions[id].thunk_address;
    }
    return 0;
}

HLEFunction* hle_get_by_id(uint32_t id) {
    if (id >= g_hle.count) return NULL;
    return &g_hle.functions[id];
}

HLEFunction* hle_get_by_address(uint32_t address) {
    if (address < THUNK_BASE_ADDRESS) return NULL;
    uint32_t offset = address - THUNK_BASE_ADDRESS;
    uint32_t id = offset / THUNK_STRIDE;
    if (id >= g_hle.count) return NULL;
    return &g_hle.functions[id];
}

void hle_set_last_error(DWORD error) {
    g_hle.last_error = error;
    // Update TEB LastErrorValue at offset 0x34
    TEB32* teb = (TEB32*)mem_to_host(DEFAULT_TEB_ADDRESS);
    if (teb) teb->LastErrorValue = error;
}

DWORD hle_get_last_error(void) {
    return g_hle.last_error;
}

// Handle Management
HANDLE handle_alloc(void* native_ptr, uint32_t type) {
    for (uint32_t i = 4; i < MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].native_ptr = native_ptr;
            s_handles[i].type = type;
            s_handles[i].in_use = true;
            return (HANDLE)(i << 2); // Handles are multiples of 4
        }
    }
    return INVALID_HANDLE_VALUE;
}

void* handle_get(HANDLE h, uint32_t type) {
    if (h == (HANDLE)(uint32_t)-10) return s_handles[1].native_ptr;
    if (h == (HANDLE)(uint32_t)-11) return s_handles[2].native_ptr;
    if (h == (HANDLE)(uint32_t)-12) return s_handles[3].native_ptr;

    uint32_t idx = ((uint32_t)h) >> 2;
    if (idx < MAX_HANDLES && s_handles[idx].in_use) {
        if (type == 0 || s_handles[idx].type == type) {
            return s_handles[idx].native_ptr;
        }
    }
    return NULL;
}

void handle_free(HANDLE h) {
    uint32_t idx = ((uint32_t)h) >> 2;
    if (idx >= 4 && idx < MAX_HANDLES && s_handles[idx].in_use) {
        s_handles[idx].in_use = false;
        s_handles[idx].native_ptr = NULL;
    }
}
