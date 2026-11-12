# General-Purpose Win32 HLE + x86 Emulator — 1-Day Master Implementation Plan (COMPLETED)

> [!IMPORTANT]
> **STRICT ARCHITECTURAL RULE FOR ALL AGENTS / MODELS**:
> **NEVER** write app-specific hacks (e.g., `if (strcmp(class_name, "Notepad") == 0)` or hardcoded menus/strings for a single `.exe`). Every feature MUST be implemented generically as part of the Win32 OS subsystem (`pe_loader.c`, `win32_dispatch.c`, `kernel32.c`, `user32.c`, `gdi32.c`, `comdlg32.c`, `msvcrt.c`, `cards.c`, `x86_core.c`) so that **any** standard Win32 PE32 binary works through the exact same code path.

---

## Status Summary (All 5 Phases Completed & Verified)

- [x] **Phase 1**: Universal PE Loader, MUI Satellite Resources (`0x20000000`), Named Resources (`pe_find_resource_ex`), and Relocations (`src/pe_loader.h`, `src/pe_loader.c`)
- [x] **Phase 2**: Stack-Safe Win32 API Dispatch (`lookup_known_stdcall_argc`), Dynamic Resolution (`GetProcAddress`), Heap/GlobalAlloc/LocalAlloc (`GMEM_ZEROINIT`/`LMEM_ZEROINIT`), `FormatMessageW`, `vswprintf`, and x86 CPU `0x66` 16-bit prefix + x87 FPU opcodes (`src/win32/win32_dispatch.c`, `src/win32/kernel32.c`, `src/win32/msvcrt.c`, `src/x86_core.c`)
- [x] **Phase 3**: Real Built-In Win32 Window Classes (`Button`, `Edit`, `Static`, `ListBox`, `ComboBox`, `ScrollBar`, `msctls_statusbar32`) with `CallWindowProcA/W` subclassing (`src/win32/user32.h`, `src/win32/user32.c`)
- [x] **Phase 4**: Generic Menu System (`RT_MENU` v0 and `MENUEX` v1 recursive parser, interactive dropdown popup rendering) & `RT_DIALOG` / `DLGTEMPLATEEX` Template Engine supporting custom dialog classes (`src/win32/user32.c`, `src/win32/gdi32.c`, `src/win32/comdlg32.c`, `src/win32/cards.c`)
- [x] **Phase 5**: End-to-End Verification across genuine Microsoft PE32 binaries (`apps/notepad10.exe`, `apps/notepad.exe`, `apps/calc.exe`, `apps/winmine.exe`, `apps/sol.exe`), multi-app interactive session switching via `[MENU]` (complete subsystem state reset across `kernel32.c`, `msvcrt.c`, `gdi32.c`, `user32.c`, `advapi32.c`), Windows 7/10 HLE test suites (`mingw32-make test_win710`), and Kindle Paperwhite 5 ARMv7 KUAL package (`dist/win32-kual-extension.tar.gz`)

---

## Phase 1: Universal PE Loader, MUI Satellite Resources, Named Resources & Relocations
**Files**: `src/pe_loader.h`, `src/pe_loader.c`

### 1.1 Companion `.mui` Resource Module Mapping
Modern Windows (Vista/7/10/11) binaries strip UI resources (`RT_MENU`, `RT_DIALOG`, `RT_STRING`, `RT_ACCELERATOR`) out of `.exe` and place them into `<dir>/<lang>/<exe_name>.mui` or `<dir>/<lang>/notepad.exe.mui` (mapped at dedicated resource VA `0x20000000`).
- **Implemented**:
  - Added `uint32_t mui_base;`, `uint32_t mui_rsrc_rva;`, and `uint32_t mui_rsrc_size;` to `win32_process_t` in `src/pe_loader.h`.
  - In `pe_load_file`, companion `.mui` files (`en-US/<exe>.mui`, `tr-TR/<exe>.mui`, `<exe>.mui`) are automatically discovered and mapped at `0x20000000`.
  - `pe_find_resource_data`, `pe_find_resource_ex`, and `pe_load_resource_string` search the `.exe` resource tree first and fall back to the `.mui` resource tree.

### 1.2 Named Resource Lookup (`pe_find_resource_ex`)
- **Implemented**:
  - Supports both integer resource IDs (`IS_INTRESOURCE(x)`) and string resource names (ASCII/UTF-16 and `#123` syntax) across `LoadMenu`, `LoadIcon`, `LoadAccelerators`, `FindResourceA/W/ExW`, and `CreateDialogParamA/W` / `DialogBoxParamA/W`.

---

## Phase 2: Stack-Safe Win32 API Dispatch, Heap & CPU Completeness
**Files**: `src/win32/win32_dispatch.c`, `src/win32/kernel32.c`, `src/win32/msvcrt.c`, `src/x86_core.c`

- **Implemented**:
  - `lookup_known_stdcall_argc` in `win32_dispatch.c` provides exact `__stdcall` argument counts for 90+ Win32 APIs so unimplemented fallbacks never corrupt `ESP`.
  - `k32_GlobalAlloc` and `k32_LocalAlloc` properly forward `(heap, flags, bytes)` to `k32_HeapAlloc` and honor `GMEM_ZEROINIT` / `LMEM_ZEROINIT`.
  - `k32_FormatMessageW` formats `%1`, `%1!s!`, `%0` insert strings from guest memory, and `crt_stdio_common_vswprintf` formats wide strings from guest `va_list`.
  - `x86_core.c` implements full `0x66` 16-bit operand-size prefix instructions and x87 FPU opcodes (`0x9B` `FWAIT`, `0xD8..0xDF`).

---

## Phase 3 & 4: Real Built-In Win32 Controls, Menus, Dialogs & GDI
**Files**: `src/win32/user32.h`, `src/win32/user32.c`, `src/win32/gdi32.h`, `src/win32/gdi32.c`, `src/win32/cards.c`

- **Implemented**:
  - Pre-registered built-in system classes (`Button`, `Edit`, `Static`, `ListBox`, `ComboBox`, `msctls_statusbar32`, `ScrollBar`) with `BUILTIN_WNDPROC_*` handlers and `CallWindowProcA/W` subclassing.
  - Recursive `RT_MENU` (v0) and `MENUEX` (v1) menu parser with interactive menu bar and dropdown popup dispatch.
  - Binary `DLGTEMPLATE` / `DLGTEMPLATEEX` parser supporting custom dialog classes (`"SciCalc"` in `calc.exe`), child control creation, `WM_CREATE`, `WM_INITDIALOG`, and `WM_SIZE`.
  - Complete GDI stock object caching (`GetStockObject`), `SelectObject` previous-handle tracking, `GetDeviceCaps` (`ASPECTX`/`ASPECTY`/`LOGPIXELSX`), `DrawTextA/W`, `DrawEdge`, and `DrawFrameControl`.
