#include "win32_dispatch.h"
#include "kernel32.h"
#include "user32.h"
#include "gdi32.h"
#include "advapi32.h"
#include "comdlg32.h"
#include "msvcrt.h"
#include "cards.h"
#include <strings.h>

#define MAX_HLE_APIS 1024

static hle_api_entry_t s_api_table[MAX_HLE_APIS];
static int s_api_count = 0;

static uint8_t lookup_known_stdcall_argc(const char *func_name) {
    if (!func_name) return 0;
    static const struct { const char *name; uint8_t argc; } k_table[] = {
        { "InitCommonControls", 0 },
        { "InitCommonControlsEx", 1 },
        { "CreateStatusWindowA", 4 },
        { "CreateStatusWindowW", 4 },
        { "DrawStatusTextA", 4 },
        { "DrawStatusTextW", 4 },
        { "MenuHelp", 7 },
        { "ShellAboutA", 4 },
        { "ShellAboutW", 4 },
        { "DragAcceptFiles", 2 },
        { "DragQueryFileA", 4 },
        { "DragQueryFileW", 4 },
        { "DragFinish", 1 },
        { "SHGetFileInfoA", 5 },
        { "SHGetFileInfoW", 5 },
        { "ShellExecuteA", 6 },
        { "ShellExecuteW", 6 },
        { "RegOpenKeyA", 3 },
        { "RegOpenKeyW", 3 },
        { "RegOpenKeyExA", 5 },
        { "RegOpenKeyExW", 5 },
        { "RegCreateKeyA", 3 },
        { "RegCreateKeyW", 3 },
        { "RegCreateKeyExA", 9 },
        { "RegCreateKeyExW", 9 },
        { "RegQueryValueA", 4 },
        { "RegQueryValueW", 4 },
        { "RegQueryValueExA", 6 },
        { "RegQueryValueExW", 6 },
        { "RegSetValueExA", 6 },
        { "RegSetValueExW", 6 },
        { "RegCloseKey", 1 },
        { "GetSaveFileNameA", 1 },
        { "GetSaveFileNameW", 1 },
        { "GetOpenFileNameA", 1 },
        { "GetOpenFileNameW", 1 },
        { "ChooseFontA", 1 },
        { "ChooseFontW", 1 },
        { "PageSetupDlgA", 1 },
        { "PageSetupDlgW", 1 },
        { "PrintDlgA", 1 },
        { "PrintDlgW", 1 },
        { "PrintDlgExA", 1 },
        { "PrintDlgExW", 1 },
        { "FindTextA", 1 },
        { "FindTextW", 1 },
        { "ReplaceTextA", 1 },
        { "ReplaceTextW", 1 },
        { "CommDlgExtendedError", 0 },
        { "WinHelpA", 4 },
        { "WinHelpW", 4 },
        { "HtmlHelpA", 4 },
        { "HtmlHelpW", 4 },
        { "ImmGetContext", 1 },
        { "ImmReleaseContext", 2 },
        { "ImmSetCompositionFontA", 2 },
        { "ImmSetCompositionFontW", 2 },
        { "ImmSetCompositionWindow", 2 },
        { "SetScrollInfo", 4 },
        { "GetScrollInfo", 3 },
        { "SetScrollPos", 4 },
        { "GetScrollPos", 2 },
        { "SetScrollRange", 5 },
        { "EnableScrollBar", 3 },
        { "ShowScrollBar", 3 },
        { "CharUpperA", 1 },
        { "CharUpperW", 1 },
        { "CharLowerA", 1 },
        { "CharLowerW", 1 },
        { "CharNextA", 1 },
        { "CharNextW", 1 },
        { "CharPrevA", 2 },
        { "CharPrevW", 2 },
        { "IsCharAlphaA", 1 },
        { "IsCharAlphaW", 1 },
        { "IsCharAlphaNumericA", 1 },
        { "IsCharAlphaNumericW", 1 },
        { "RtlDisownModuleHeapAllocation", 2 },
        { "CreateDialogParamA", 5 },
        { "CreateDialogParamW", 5 },
        { "CreateDialogIndirectParamA", 5 },
        { "CreateDialogIndirectParamW", 5 },
        { "OpenSemaphoreW", 3 },
        { "UnhandledExceptionFilter", 1 },
        { "RaiseException", 4 },
        { "SetLastError", 1 },
        { "SetThreadpoolTimer", 4 },
        { "WaitForThreadpoolTimerCallbacks", 2 },
        { "CreateThreadpoolTimer", 3 },
        { "CloseThreadpoolTimer", 1 },
        { "CreateProcessW", 10 },
        { "TerminateProcess", 2 },
        { "FindNLSString", 7 },
        { "GetLocaleInfoW", 4 },
        { "GetACP", 0 },
        { "OutputDebugStringW", 1 },
        { "DebugBreak", 0 },
        { "PropVariantClear", 1 },
        { "CoWaitForMultipleHandles", 5 },
        { "CoCreateFreeThreadedMarshaler", 2 },
        { "RegSetKeyValueW", 6 },
        { "PathIsFileSpecW", 1 },
        { "PathFileExistsW", 1 },
        { "PathFindExtensionW", 1 },
        { "WindowsCreateString", 3 },
        { "WindowsGetStringRawBuffer", 2 },
        { "RegGetValueW", 7 },
        { "RegDeleteKeyExW", 4 },
        { "RegEnumValueW", 8 },
        { "RegQueryInfoKeyW", 12 },
        { "LocalUnlock", 1 },
        { "DeleteFileW", 1 },
        { "GetFileAttributesW", 1 },
        { "SetEndOfFile", 1 },
        { "GetFileAttributesExW", 3 },
        { "GetFileInformationByHandle", 2 },
        { "FindClose", 1 },
        { "FindFirstFileW", 2 },
        { "CreateFileW", 7 },
        { "GetDiskFreeSpaceExW", 4 },
        { "GetFullPathNameW", 4 },
        { "CreateDirectoryW", 2 },
        { "SHStrDupW", 2 },
        { "GetTokenInformation", 5 },
        { "GetCurrentDirectoryW", 2 },
        { "SetCurrentDirectoryW", 1 },
        { "FoldStringW", 5 },
        { "CompareStringOrdinal", 5 },
        { "K32GetModuleFileNameExW", 4 },
        { "GetUserDefaultUILanguage", 0 },
        { "GetSystemTimeAsFileTime", 1 },
        { "GetLocalTime", 1 },
        { "GetDateFormatW", 6 },
        { "GetTimeFormatW", 6 },
        { "MapViewOfFile", 5 },
        { "CreateFileMappingW", 6 },
        { "UnmapViewOfFile", 1 },
        { "EventSetInformation", 5 },
        { "EventWriteTransfer", 7 },
        { "IsTextUnicode", 3 },
        { "GetProcessMitigationPolicy", 4 },
        { "QueryPerformanceCounter", 1 },
        { "InitializeSListHead", 1 },
        { "SetRestrictedErrorInfo", 1 },
        { "RoGetMatchingRestrictedErrorInfo", 2 },
        { "ImageList_Create", 5 },
        { "ImageList_SetBkColor", 2 },
        { "ImageList_ReplaceIcon", 3 },
        { "ImageList_Draw", 6 },
        { "ImageList_GetIconSize", 3 },
        { "ImageList_Destroy", 1 },
        { "DelayLoadFailureHook", 2 },
        { "DecryptFileW", 2 },
        { "DuplicateEncryptionInfoFile", 5 },
        { "GetFileTitleW", 3 },
        { "PSGetPropertyDescriptionListFromString", 3 },
        { "PropVariantToStringVectorAlloc", 3 },
        { "SHCreateItemFromParsingName", 4 },
        { "SHGetKnownFolderPath", 4 },
        { "SHAddToRecentDocs", 2 },
        { "ClosePrinter", 1 },
        { "GetPrinterDriverW", 6 },
        { "OpenPrinterW", 3 },
        { "FindMimeFromData", 8 },
        { NULL, 0 }
    };
    for (int i = 0; k_table[i].name != NULL; i++) {
        if (strcasecmp(func_name, k_table[i].name) == 0) {
            return k_table[i].argc;
        }
    }
    return 0;
}

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count) {
    if (s_api_count >= MAX_HLE_APIS) {
        LOGE("Exceeded MAX_HLE_APIS limit");
        return;
    }
    s_api_table[s_api_count].dll_name = dll;
    s_api_table[s_api_count].func_name = name;
    s_api_table[s_api_count].ordinal = ordinal;
    s_api_table[s_api_count].handler = handler;
    s_api_table[s_api_count].arg_count = arg_count;
    s_api_count++;
}

static uint32_t fallback_unimplemented_api(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    LOGW("Unimplemented Win32 API called!");
    return 0;
}

bool win32_is_api_implemented(int api_id) {
    if (api_id <= 0 || api_id >= s_api_count) return false;
    return s_api_table[api_id].handler != fallback_unimplemented_api || s_api_table[api_id].arg_count > 0;
}

int win32_register_or_find_api(const char *dll_name, const char *func_name, uint16_t ordinal) {
    /* 1. Try finding exact match in registered table */
    for (int i = 0; i < s_api_count; i++) {
        if (strcasecmp(s_api_table[i].dll_name, dll_name) == 0) {
            if (func_name && s_api_table[i].func_name && strcasecmp(s_api_table[i].func_name, func_name) == 0) {
                return i;
            }
            if (ordinal != 0 && s_api_table[i].ordinal == ordinal) {
                return i;
            }
        }
    }

    /* 1.5. If func_name is given, search across all registered APIs
     * (resolves ApiSet forwards such as api-ms-win-core-* -> KERNEL32, api-ms-win-crt-* -> msvcrt, etc.) */
    if (func_name) {
        for (int i = 0; i < s_api_count; i++) {
            if (s_api_table[i].func_name && strcasecmp(s_api_table[i].func_name, func_name) == 0 &&
                s_api_table[i].handler != fallback_unimplemented_api) {
                return i;
            }
        }

        /* 1.6. Check if modern UCRT private prefix '_o_' is present (e.g. _o_malloc -> malloc) */
        if (strncmp(func_name, "_o_", 3) == 0) {
            const char *clean_name = func_name + 3;
            for (int i = 0; i < s_api_count; i++) {
                if (s_api_table[i].func_name && strcasecmp(s_api_table[i].func_name, clean_name) == 0 &&
                    s_api_table[i].handler != fallback_unimplemented_api) {
                    return i;
                }
            }
        }
    }

    /* 2. Fallback: dynamically register stub */
    if (s_api_count < MAX_HLE_APIS) {
        int idx = s_api_count++;
        s_api_table[idx].dll_name = strdup(dll_name);
        s_api_table[idx].func_name = func_name ? strdup(func_name) : "(by-ordinal)";
        s_api_table[idx].ordinal = ordinal;
        s_api_table[idx].handler = fallback_unimplemented_api;
        s_api_table[idx].arg_count = lookup_known_stdcall_argc(func_name);
        LOGW("Registered placeholder stub for [%s]!%s (id=%d, argc=%u)", dll_name, s_api_table[idx].func_name, idx, s_api_table[idx].arg_count);
        return idx;
    }

    return 0;
}

bool win32_dispatch_api(win32_process_t *proc, uint32_t api_id) {
    if (!proc || api_id >= (uint32_t)s_api_count) return false;

    hle_api_entry_t *entry = &s_api_table[api_id];
    uint32_t esp = proc->cpu.regs.esp;

    /* Read return address at [ESP] */
    uint32_t ret_addr = 0;
    if (!x86_mem_read32(&proc->cpu, esp, &ret_addr)) return false;

    /* Read up to 16 arguments from [ESP + 4], [ESP + 8], etc. */
    uint32_t args[16] = {0};
    for (int i = 0; i < entry->arg_count && i < 16; i++) {
        x86_mem_read32(&proc->cpu, esp + 4 + i * 4, &args[i]);
    }

    if (entry->handler == fallback_unimplemented_api) {
        LOGW("API CALL [UNIMPLEMENTED]: [%s]!%s (id=%u, ret=0x%08X)",
             entry->dll_name, entry->func_name, api_id, ret_addr);
    } else if (strcmp(entry->func_name, "GetMessageW") != 0 &&
               strcmp(entry->func_name, "GetMessageA") != 0 &&
               strcmp(entry->func_name, "PeekMessageW") != 0 &&
               strcmp(entry->func_name, "PeekMessageA") != 0 &&
               strcmp(entry->func_name, "TranslateAcceleratorW") != 0 &&
               strcmp(entry->func_name, "TranslateAcceleratorA") != 0 &&
               strcmp(entry->func_name, "TranslateMessage") != 0 &&
               strcmp(entry->func_name, "DispatchMessageW") != 0 &&
               strcmp(entry->func_name, "DispatchMessageA") != 0 &&
               strcmp(entry->func_name, "DefWindowProcW") != 0 &&
               strcmp(entry->func_name, "DefWindowProcA") != 0 &&
               strcmp(entry->func_name, "IsDialogMessageW") != 0 &&
               strcmp(entry->func_name, "IsDialogMessageA") != 0) {
        LOGI("API CALL: [%s]!%s (ret=0x%08X, arg0=0x%08X)",
             entry->dll_name, entry->func_name, ret_addr, args[0]);
    }

    /* Execute native HLE handler */
    uint32_t ret_val = 0;
    if (entry->handler) {
        ret_val = entry->handler(proc, args);
    }

    /* Return value placed in EAX */
    proc->cpu.regs.eax = ret_val;

    /* Stack unwinding: msvcrt and CRT functions are __cdecl (caller pops args), Windows DLLs are __stdcall */
    bool is_cdecl = (strcasecmp(entry->dll_name, "msvcrt.dll") == 0 ||
                     strcasecmp(entry->dll_name, "ucrtbase.dll") == 0 ||
                     strcasecmp(entry->dll_name, "vcruntime140.dll") == 0 ||
                     strncasecmp(entry->dll_name, "api-ms-win-crt-", 15) == 0 ||
                     strncmp(entry->func_name, "_o_", 3) == 0 ||
                     strncmp(entry->func_name, "__stdio_", 8) == 0 ||
                     strcasecmp(entry->func_name, "wsprintfW") == 0 ||
                     strcasecmp(entry->func_name, "wsprintfA") == 0);
    if (is_cdecl) {
        proc->cpu.regs.esp += 4; /* pop return address only */
    } else {
        proc->cpu.regs.esp += 4 + (entry->arg_count * 4); /* __stdcall */
    }
    proc->cpu.regs.eip = ret_addr;

    return true;
}

void win32_init_subsystem(win32_process_t *proc) {
    (void)proc;
    s_api_count = 0;
    kernel32_init();
    gdi32_init();
    user32_init();
    advapi32_init();
    comdlg32_init();
    msvcrt_init();
    cards_init();
    LOGI("Win32 Subsystem initialized with %d native API handlers", s_api_count);
}
