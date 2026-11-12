#include "comdlg32.h"
#include "user32.h"
#include "win32_dispatch.h"

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

static uint32_t cdlg_GetOpenFileNameA(win32_process_t *proc, uint32_t *args) {
    uint8_t *ofn = guest_va_to_host(&proc->mem, args[0]);
    if (!ofn) return 0;

    uint32_t file_ptr = *(uint32_t *)(ofn + 12); /* lpstrFile */
    char *out_path = (char *)guest_va_to_host(&proc->mem, file_ptr);
    if (out_path) {
        strcpy(out_path, "C:\\document.txt");
        LOGI("GetOpenFileNameA returning: %s", out_path);
        return 1;
    }
    return 0;
}

static uint32_t cdlg_GetSaveFileNameA(win32_process_t *proc, uint32_t *args) {
    return cdlg_GetOpenFileNameA(proc, args);
}

static uint32_t shell_ExtractIconA(win32_process_t *proc, uint32_t *args) {
    const char *exe_path = (const char *)guest_va_to_host(&proc->mem, args[1]);
    int icon_idx = (int)args[2];
    if (!exe_path) return 0;
    return (uint32_t)user_extract_icon(exe_path, icon_idx);
}

static uint32_t shell_ShellAboutA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    LOGI("SHELL32!ShellAboutA called");
    return 1;
}

static uint32_t cdlg_dummy_zero(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

void comdlg32_init(void) {
    win32_register_api("COMDLG32.DLL", "GetOpenFileNameA", 0, cdlg_GetOpenFileNameA, 1);
    win32_register_api("COMDLG32.DLL", "GetSaveFileNameA", 0, cdlg_GetSaveFileNameA, 1);
    win32_register_api("COMDLG32.DLL", "PageSetupDlgA", 0, cdlg_dummy_zero, 1);
    win32_register_api("COMDLG32.DLL", "FindTextA", 0, cdlg_dummy_zero, 1);
    win32_register_api("COMDLG32.DLL", "ReplaceTextA", 0, cdlg_dummy_zero, 1);
    win32_register_api("COMDLG32.DLL", "ChooseFontA", 0, cdlg_dummy_zero, 1);
    win32_register_api("COMDLG32.DLL", "GetFileTitleA", 0, cdlg_dummy_zero, 3);
    win32_register_api("COMDLG32.DLL", "CommDlgExtendedError", 0, cdlg_dummy_zero, 0);

    win32_register_api("SHELL32.DLL", "ExtractIconA", 0, shell_ExtractIconA, 3);
    win32_register_api("SHELL32.DLL", "ShellAboutA", 0, shell_ShellAboutA, 4);
}
