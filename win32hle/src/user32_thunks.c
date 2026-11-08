#include "hle.h"
#include "memory.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_msg_count = 0;

static uint32_t hle_RegisterWindowMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    static uint32_t s_msg_id = 0xC001;
    if (args[0]) {
        const uint16_t* wname = (const uint16_t*)mem_to_host(args[0]);
        char name[128] = {0};
        for (int i = 0; i < 127 && wname && wname[i]; i++) name[i] = (char)wname[i];
        printf("[USER32] RegisterWindowMessageW: '%s' -> 0x%04X\n", name, s_msg_id);
    }
    return s_msg_id++;
}

static uint32_t hle_LoadCursorW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x10001;
}

static uint32_t hle_LoadIconW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x20001;
}

static uint32_t hle_LoadImageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x20002;
}

static uint32_t hle_LoadAcceleratorsW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x30001;
}

static uint32_t hle_RegisterClassExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    printf("[USER32] RegisterClassExW (WNDCLASSEXW at 0x%08X)\n", args[0]);
    return 0xC042; // Valid ATOM
}

static uint32_t hle_CreateWindowExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t class_ptr = args[1];
    uint32_t title_ptr = args[2];
    char class_buf[128] = {0};
    char title_buf[128] = {0};
    if (class_ptr > 0xFFFF) {
        const uint16_t* wc = (const uint16_t*)mem_to_host(class_ptr);
        for (int i = 0; i < 127 && wc && wc[i]; i++) class_buf[i] = (char)wc[i];
    } else {
        snprintf(class_buf, sizeof(class_buf), "atom#%u", class_ptr);
    }
    if (title_ptr) {
        const uint16_t* wt = (const uint16_t*)mem_to_host(title_ptr);
        for (int i = 0; i < 127 && wt && wt[i]; i++) title_buf[i] = (char)wt[i];
    }
    printf("[USER32] CreateWindowExW: Class='%s', Title='%s' -> HWND 0x50001\n", class_buf, title_buf);
    return 0x50001;
}

static uint32_t hle_ShowWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    printf("[USER32] ShowWindow: HWND=0x%08X, nCmdShow=%u\n", args[0], args[1]);
    return 1;
}

static uint32_t hle_UpdateWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SetWindowTextW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[1]) {
        const uint16_t* wt = (const uint16_t*)mem_to_host(args[1]);
        char title[128] = {0};
        for (int i = 0; i < 127 && wt && wt[i]; i++) title[i] = (char)wt[i];
        printf("[USER32] SetWindowTextW: '%s'\n", title);
    }
    return 1;
}

static uint32_t hle_GetClientRect(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t rect_ptr = args[1];
    if (rect_ptr) {
        // RECT: left=0, top=0, right=800, bottom=600
        mem_write_u32(rect_ptr, 0);
        mem_write_u32(rect_ptr + 4, 0);
        mem_write_u32(rect_ptr + 8, 800);
        mem_write_u32(rect_ptr + 12, 600);
    }
    return 1;
}

static uint32_t hle_GetWindowPlacement(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t lpwndpl = args[1];
    if (lpwndpl) {
        // WINDOWPLACEMENT
        mem_write_u32(lpwndpl, 44); // length
        mem_write_u32(lpwndpl + 4, 0); // flags
        mem_write_u32(lpwndpl + 8, 1); // showCmd: SW_SHOWNORMAL
        // rcNormalPosition
        mem_write_u32(lpwndpl + 28, 100); // left
        mem_write_u32(lpwndpl + 32, 100); // top
        mem_write_u32(lpwndpl + 36, 900); // right
        mem_write_u32(lpwndpl + 40, 700); // bottom
    }
    return 1;
}

static uint32_t hle_SetWindowPlacement(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_MoveWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SetWindowPos(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetDpiForWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 96; // Standard 96 DPI (100% scaling)
}

static uint32_t hle_GetDpiForMonitor(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[2]) mem_write_u32(args[2], 96);
    if (args[3]) mem_write_u32(args[3], 96);
    return 0; // S_OK
}

static uint32_t hle_SetThreadDpiAwarenessContext(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_GetMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t msg_ptr = args[0];
    s_msg_count++;
    // Feed one WM_CREATE or WM_PAINT then exit message loop with WM_QUIT (0)
    if (s_msg_count <= 2 && msg_ptr) {
        // MSG: hwnd, message, wParam, lParam, time, pt
        mem_write_u32(msg_ptr, 0x50001);
        mem_write_u32(msg_ptr + 4, (s_msg_count == 1) ? 0x000F : 0x0012); // WM_PAINT then WM_QUIT
        mem_write_u32(msg_ptr + 8, 0);
        mem_write_u32(msg_ptr + 12, 0);
        mem_write_u32(msg_ptr + 16, 1000);
        return 1;
    }
    printf("[USER32] GetMessageW: Message loop terminated cleanly (WM_QUIT)\n");
    return 0; // WM_QUIT exits the message loop
}

static uint32_t hle_PeekMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0; // No pending messages
}

static uint32_t hle_TranslateMessage(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_DispatchMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_TranslateAcceleratorW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_IsDialogMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_DefWindowProcW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_PostQuitMessage(struct CPUState* cpu, uint32_t* args) {
    cpu->halted = true;
    cpu->exit_code = args[0];
    return 0;
}

static uint32_t hle_PostMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SendMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_DestroyWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetSystemMenu(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x60001;
}

static uint32_t hle_GetMenu(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x60002;
}

static uint32_t hle_GetSubMenu(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x60003;
}

static uint32_t hle_CheckMenuItem(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_EnableMenuItem(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_GetDC(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x70001;
}

static uint32_t hle_ReleaseDC(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetDeviceCaps(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t index = args[1];
    if (index == 88 || index == 90) return 96; // LOGPIXELSX, LOGPIXELSY
    if (index == 8) return 1920; // HORZRES
    if (index == 10) return 1080; // VERTRES
    return 96;
}

static uint32_t hle_CreateFontIndirectW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x80001;
}

static uint32_t hle_SelectObject(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x80001;
}

static uint32_t hle_DeleteObject(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SetBkMode(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_CreateStatusWindowW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50002;
}

static uint32_t hle_MessageBoxW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t text_ptr = args[1];
    uint32_t caption_ptr = args[2];
    char text[256] = {0};
    char caption[128] = {0};
    if (text_ptr) {
        const uint16_t* wt = (const uint16_t*)mem_to_host(text_ptr);
        for (int i = 0; i < 255 && wt && wt[i]; i++) text[i] = (char)wt[i];
    }
    if (caption_ptr) {
        const uint16_t* wc = (const uint16_t*)mem_to_host(caption_ptr);
        for (int i = 0; i < 127 && wc && wc[i]; i++) caption[i] = (char)wc[i];
    }
    printf("[USER32] MessageBoxW: [%s] %s\n", caption, text);
    return 1; // IDOK
}

static uint32_t hle_CharUpperW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t ptr = args[0];
    if (ptr <= 0xFFFF) {
        uint16_t c = (uint16_t)ptr;
        if (c >= 'a' && c <= 'z') c -= 32;
        return (uint32_t)c;
    }
    uint16_t* s = (uint16_t*)mem_to_host(ptr);
    if (s) {
        for (int i = 0; s[i]; i++) {
            if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32;
        }
    }
    return ptr;
}

static uint32_t hle_CharNextW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    uint32_t ptr = args[0];
    if (!ptr) return 0;
    uint16_t* s = (uint16_t*)mem_to_host(ptr);
    if (s && *s) return ptr + 2;
    return ptr;
}

static uint32_t hle_SetCursor(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x10001;
}

static uint32_t hle_GetFocus(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50001;
}

static uint32_t hle_SetFocus(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50001;
}

static uint32_t hle_SetActiveWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50001;
}

static uint32_t hle_GetForegroundWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50001;
}

static uint32_t hle_IsIconic(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_MessageBeep(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetDlgCtrlID(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 15;
}

static uint32_t hle_RedrawWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetKeyboardLayout(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x04090409;
}

static uint32_t hle_SetWinEventHook(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x70001;
}

static uint32_t hle_UnhookWinEvent(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_OpenClipboard(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_CloseClipboard(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_IsClipboardFormatAvailable(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_SetDlgItemTextW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_GetDlgItemTextW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[2] && args[3]) mem_write_u16(args[2], 0);
    return 0;
}

static uint32_t hle_EndDialog(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SendDlgItemMessageW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_SetScrollPos(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_InvalidateRect(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_SetWindowLongW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_GetWindowLongW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_MonitorFromWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x80001;
}

static uint32_t hle_GetWindowTextLengthW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0;
}

static uint32_t hle_GetWindowTextW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu;
    if (args[1] && args[2]) mem_write_u16(args[1], 0);
    return 0;
}

static uint32_t hle_EnableWindow(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_CreateDialogParamW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 0x50003;
}

static uint32_t hle_DialogBoxParamW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

static uint32_t hle_DrawTextExW(struct CPUState* cpu, uint32_t* args) {
    (void)cpu; (void)args;
    return 1;
}

void register_user32_apis(void) {
    hle_register("user32.dll", "RegisterWindowMessageW", 0, hle_RegisterWindowMessageW, 1, false);
    hle_register("user32.dll", "LoadCursorW", 0, hle_LoadCursorW, 2, false);
    hle_register("user32.dll", "LoadIconW", 0, hle_LoadIconW, 2, false);
    hle_register("user32.dll", "LoadImageW", 0, hle_LoadImageW, 6, false);
    hle_register("user32.dll", "LoadAcceleratorsW", 0, hle_LoadAcceleratorsW, 2, false);
    hle_register("user32.dll", "RegisterClassExW", 0, hle_RegisterClassExW, 1, false);
    hle_register("user32.dll", "CreateWindowExW", 0, hle_CreateWindowExW, 12, false);
    hle_register("user32.dll", "ShowWindow", 0, hle_ShowWindow, 2, false);
    hle_register("user32.dll", "UpdateWindow", 0, hle_UpdateWindow, 1, false);
    hle_register("user32.dll", "SetWindowTextW", 0, hle_SetWindowTextW, 2, false);
    hle_register("user32.dll", "GetClientRect", 0, hle_GetClientRect, 2, false);
    hle_register("user32.dll", "GetWindowPlacement", 0, hle_GetWindowPlacement, 2, false);
    hle_register("user32.dll", "SetWindowPlacement", 0, hle_SetWindowPlacement, 2, false);
    hle_register("user32.dll", "MoveWindow", 0, hle_MoveWindow, 6, false);
    hle_register("user32.dll", "SetWindowPos", 0, hle_SetWindowPos, 7, false);
    hle_register("user32.dll", "GetDpiForWindow", 0, hle_GetDpiForWindow, 1, false);
    hle_register("shcore.dll", "GetDpiForMonitor", 0, hle_GetDpiForMonitor, 4, false);
    hle_register("user32.dll", "SetThreadDpiAwarenessContext", 0, hle_SetThreadDpiAwarenessContext, 1, false);
    hle_register("user32.dll", "GetMessageW", 0, hle_GetMessageW, 4, false);
    hle_register("user32.dll", "PeekMessageW", 0, hle_PeekMessageW, 5, false);
    hle_register("user32.dll", "TranslateMessage", 0, hle_TranslateMessage, 1, false);
    hle_register("user32.dll", "DispatchMessageW", 0, hle_DispatchMessageW, 1, false);
    hle_register("user32.dll", "TranslateAcceleratorW", 0, hle_TranslateAcceleratorW, 3, false);
    hle_register("user32.dll", "IsDialogMessageW", 0, hle_IsDialogMessageW, 2, false);
    hle_register("user32.dll", "DefWindowProcW", 0, hle_DefWindowProcW, 4, false);
    hle_register("user32.dll", "PostQuitMessage", 0, hle_PostQuitMessage, 1, false);
    hle_register("user32.dll", "PostMessageW", 0, hle_PostMessageW, 4, false);
    hle_register("user32.dll", "SendMessageW", 0, hle_SendMessageW, 4, false);
    hle_register("user32.dll", "DestroyWindow", 0, hle_DestroyWindow, 1, false);
    hle_register("user32.dll", "GetSystemMenu", 0, hle_GetSystemMenu, 2, false);
    hle_register("user32.dll", "GetMenu", 0, hle_GetMenu, 1, false);
    hle_register("user32.dll", "GetSubMenu", 0, hle_GetSubMenu, 2, false);
    hle_register("user32.dll", "CheckMenuItem", 0, hle_CheckMenuItem, 3, false);
    hle_register("user32.dll", "EnableMenuItem", 0, hle_EnableMenuItem, 3, false);
    hle_register("user32.dll", "GetDC", 0, hle_GetDC, 1, false);
    hle_register("user32.dll", "ReleaseDC", 0, hle_ReleaseDC, 2, false);
    hle_register("gdi32.dll", "GetDeviceCaps", 0, hle_GetDeviceCaps, 2, false);
    hle_register("gdi32.dll", "CreateFontIndirectW", 0, hle_CreateFontIndirectW, 1, false);
    hle_register("gdi32.dll", "SelectObject", 0, hle_SelectObject, 2, false);
    hle_register("gdi32.dll", "DeleteObject", 0, hle_DeleteObject, 1, false);
    hle_register("gdi32.dll", "SetBkMode", 0, hle_SetBkMode, 2, false);
    hle_register("comctl32.dll", "CreateStatusWindowW", 0, hle_CreateStatusWindowW, 4, false);
    hle_register("user32.dll", "MessageBoxW", 0, hle_MessageBoxW, 4, false);
    hle_register("user32.dll", "CharUpperW", 0, hle_CharUpperW, 1, false);
    hle_register("user32.dll", "CharNextW", 0, hle_CharNextW, 1, false);
    hle_register("user32.dll", "SetCursor", 0, hle_SetCursor, 1, false);
    hle_register("user32.dll", "GetFocus", 0, hle_GetFocus, 0, false);
    hle_register("user32.dll", "SetFocus", 0, hle_SetFocus, 1, false);
    hle_register("user32.dll", "SetActiveWindow", 0, hle_SetActiveWindow, 1, false);
    hle_register("user32.dll", "GetForegroundWindow", 0, hle_GetForegroundWindow, 0, false);
    hle_register("user32.dll", "IsIconic", 0, hle_IsIconic, 1, false);
    hle_register("user32.dll", "MessageBeep", 0, hle_MessageBeep, 1, false);
    hle_register("user32.dll", "GetDlgCtrlID", 0, hle_GetDlgCtrlID, 1, false);
    hle_register("user32.dll", "RedrawWindow", 0, hle_RedrawWindow, 4, false);
    hle_register("user32.dll", "GetKeyboardLayout", 0, hle_GetKeyboardLayout, 1, false);
    hle_register("user32.dll", "SetWinEventHook", 0, hle_SetWinEventHook, 7, false);
    hle_register("user32.dll", "UnhookWinEvent", 0, hle_UnhookWinEvent, 1, false);
    hle_register("user32.dll", "OpenClipboard", 0, hle_OpenClipboard, 1, false);
    hle_register("user32.dll", "CloseClipboard", 0, hle_CloseClipboard, 0, false);
    hle_register("user32.dll", "IsClipboardFormatAvailable", 0, hle_IsClipboardFormatAvailable, 1, false);
    hle_register("user32.dll", "SetDlgItemTextW", 0, hle_SetDlgItemTextW, 3, false);
    hle_register("user32.dll", "GetDlgItemTextW", 0, hle_GetDlgItemTextW, 4, false);
    hle_register("user32.dll", "EndDialog", 0, hle_EndDialog, 2, false);
    hle_register("user32.dll", "SendDlgItemMessageW", 0, hle_SendDlgItemMessageW, 5, false);
    hle_register("user32.dll", "SetScrollPos", 0, hle_SetScrollPos, 4, false);
    hle_register("user32.dll", "InvalidateRect", 0, hle_InvalidateRect, 3, false);
    hle_register("user32.dll", "SetWindowLongW", 0, hle_SetWindowLongW, 3, false);
    hle_register("user32.dll", "GetWindowLongW", 0, hle_GetWindowLongW, 2, false);
    hle_register("user32.dll", "MonitorFromWindow", 0, hle_MonitorFromWindow, 2, false);
    hle_register("user32.dll", "GetWindowTextLengthW", 0, hle_GetWindowTextLengthW, 1, false);
    hle_register("user32.dll", "GetWindowTextW", 0, hle_GetWindowTextW, 3, false);
    hle_register("user32.dll", "EnableWindow", 0, hle_EnableWindow, 2, false);
    hle_register("user32.dll", "CreateDialogParamW", 0, hle_CreateDialogParamW, 5, false);
    hle_register("user32.dll", "DialogBoxParamW", 0, hle_DialogBoxParamW, 5, false);
    hle_register("user32.dll", "DrawTextExW", 0, hle_DrawTextExW, 6, false);
}
