#include "advapi32.h"
#include "win32_dispatch.h"

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

#define MAX_REG_KEYS 32
typedef struct {
    char name[64];
    uint32_t dword_val;
    char str_val[128];
    bool is_string;
} reg_entry_t;

static reg_entry_t s_reg[MAX_REG_KEYS];
static int s_reg_count = 0;

static uint32_t adv_RegOpenKeyExA(win32_process_t *proc, uint32_t *args) {
    /* H_HKEY hKey = (H_HKEY)args[0]; */
    const char *sub_key = (const char *)guest_va_to_host(&proc->mem, args[1]);
    uint32_t phkResult = args[4];

    LOGI("RegOpenKeyExA: %s", sub_key ? sub_key : "(null)");
    if (phkResult) {
        x86_mem_write32(&proc->cpu, phkResult, 0x00000001); /* Synthetic open key */
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegOpenKeyExW(win32_process_t *proc, uint32_t *args) {
    uint32_t phkResult = args[4];
    if (phkResult) {
        x86_mem_write32(&proc->cpu, phkResult, 0x00000001);
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegCreateKeyW(win32_process_t *proc, uint32_t *args) {
    uint32_t phkResult = args[2];
    if (phkResult) {
        x86_mem_write32(&proc->cpu, phkResult, 0x00000001);
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegGetValueW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 2; /* ERROR_FILE_NOT_FOUND */
}

static uint32_t adv_RegCreateKeyExA(win32_process_t *proc, uint32_t *args) {
    uint32_t phkResult = args[7];
    if (phkResult) {
        x86_mem_write32(&proc->cpu, phkResult, 0x00000001);
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegQueryValueExA(win32_process_t *proc, uint32_t *args) {
    const char *val_name = (const char *)guest_va_to_host(&proc->mem, args[1]);
    uint32_t lpType = args[3];
    uint32_t lpData = args[4];
    uint32_t lpcbData = args[5];

    if (!val_name) val_name = "";
    LOGD("RegQueryValueExA: '%s'", val_name);

    /* Lookup existing value in table */
    for (int i = 0; i < s_reg_count; i++) {
        if (strcasecmp(s_reg[i].name, val_name) == 0) {
            if (s_reg[i].is_string) {
                if (lpType) x86_mem_write32(&proc->cpu, lpType, 1); /* REG_SZ */
                uint8_t *dest = guest_va_to_host(&proc->mem, lpData);
                if (dest) strcpy((char *)dest, s_reg[i].str_val);
                if (lpcbData) x86_mem_write32(&proc->cpu, lpcbData, (uint32_t)strlen(s_reg[i].str_val) + 1);
            } else {
                if (lpType) x86_mem_write32(&proc->cpu, lpType, 4); /* REG_DWORD */
                if (lpData) x86_mem_write32(&proc->cpu, lpData, s_reg[i].dword_val);
                if (lpcbData) x86_mem_write32(&proc->cpu, lpcbData, 4);
            }
            return 0; /* ERROR_SUCCESS */
        }
    }

    return 2; /* ERROR_FILE_NOT_FOUND */
}

static uint32_t adv_RegSetValueExA(win32_process_t *proc, uint32_t *args) {
    const char *val_name = (const char *)guest_va_to_host(&proc->mem, args[1]);
    uint32_t type = args[3];
    uint32_t lpData = args[4];
    uint32_t cbData = args[5];

    if (!val_name) val_name = "";
    LOGI("RegSetValueExA: '%s' (type=%u, size=%u)", val_name, type, cbData);

    int idx = -1;
    for (int i = 0; i < s_reg_count; i++) {
        if (strcasecmp(s_reg[i].name, val_name) == 0) { idx = i; break; }
    }
    if (idx == -1 && s_reg_count < MAX_REG_KEYS) {
        idx = s_reg_count++;
    }

    if (idx >= 0) {
        strncpy(s_reg[idx].name, val_name, sizeof(s_reg[idx].name) - 1);
        if (type == 4) { /* REG_DWORD */
            uint32_t val = 0;
            x86_mem_read32(&proc->cpu, lpData, &val);
            s_reg[idx].dword_val = val;
            s_reg[idx].is_string = false;
        } else {
            const char *src = (const char *)guest_va_to_host(&proc->mem, lpData);
            if (src) strncpy(s_reg[idx].str_val, src, sizeof(s_reg[idx].str_val) - 1);
            s_reg[idx].is_string = true;
        }
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegCloseKey(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* ERROR_SUCCESS */
}

static void wchar_to_ascii_adv(const uint16_t *src, char *dst, size_t max_len) {
    if (!src || !dst || max_len == 0) return;
    size_t i = 0;
    while (src[i] != 0 && i < max_len - 1) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

static uint32_t adv_RegCreateKeyExW(win32_process_t *proc, uint32_t *args) {
    uint32_t phkResult = args[7];
    if (phkResult) {
        x86_mem_write32(&proc->cpu, phkResult, 0x00000001);
    }
    return 0; /* ERROR_SUCCESS */
}

static uint32_t adv_RegQueryValueExW(win32_process_t *proc, uint32_t *args) {
    uint32_t name_va = args[1];
    uint32_t lpType = args[3];
    uint32_t lpData = args[4];
    uint32_t lpcbData = args[5];

    char val_name[64] = {0};
    if (name_va) {
        uint16_t wname[64] = {0};
        guest_mem_read(&proc->mem, name_va, wname, sizeof(wname));
        wchar_to_ascii_adv(wname, val_name, sizeof(val_name));
    }
    LOGI("RegQueryValueExW: '%s'", val_name);

    for (int i = 0; i < s_reg_count; i++) {
        if (strcasecmp(s_reg[i].name, val_name) == 0) {
            if (s_reg[i].is_string) {
                if (lpType) x86_mem_write32(&proc->cpu, lpType, 1);
                uint16_t wbuf[128] = {0};
                for (size_t k = 0; k < strlen(s_reg[i].str_val); k++) {
                    wbuf[k] = (uint16_t)s_reg[i].str_val[k];
                }
                uint32_t sz_bytes = (uint32_t)(strlen(s_reg[i].str_val) + 1) * 2;
                if (lpData) guest_mem_write(&proc->mem, lpData, wbuf, sz_bytes);
                if (lpcbData) x86_mem_write32(&proc->cpu, lpcbData, sz_bytes);
            } else {
                if (lpType) x86_mem_write32(&proc->cpu, lpType, 4);
                if (lpData) x86_mem_write32(&proc->cpu, lpData, s_reg[i].dword_val);
                if (lpcbData) x86_mem_write32(&proc->cpu, lpcbData, 4);
            }
            return 0; /* ERROR_SUCCESS */
        }
    }
    return 2; /* ERROR_FILE_NOT_FOUND */
}

static uint32_t adv_RegSetValueExW(win32_process_t *proc, uint32_t *args) {
    uint32_t name_va = args[1];
    uint32_t type = args[3];
    uint32_t lpData = args[4];
    uint32_t cbData = args[5];

    char val_name[64] = {0};
    if (name_va) {
        uint16_t wname[64] = {0};
        guest_mem_read(&proc->mem, name_va, wname, sizeof(wname));
        wchar_to_ascii_adv(wname, val_name, sizeof(val_name));
    }
    LOGI("RegSetValueExW: '%s' (type=%u, size=%u)", val_name, type, cbData);

    int idx = -1;
    for (int i = 0; i < s_reg_count; i++) {
        if (strcasecmp(s_reg[i].name, val_name) == 0) { idx = i; break; }
    }
    if (idx == -1 && s_reg_count < MAX_REG_KEYS) {
        idx = s_reg_count++;
    }

    if (idx >= 0) {
        strncpy(s_reg[idx].name, val_name, sizeof(s_reg[idx].name) - 1);
        if (type == 4) {
            uint32_t val = 0;
            x86_mem_read32(&proc->cpu, lpData, &val);
            s_reg[idx].dword_val = val;
            s_reg[idx].is_string = false;
        } else {
            uint16_t wbuf[128] = {0};
            guest_mem_read(&proc->mem, lpData, wbuf, sizeof(wbuf));
            wchar_to_ascii_adv(wbuf, s_reg[idx].str_val, sizeof(s_reg[idx].str_val));
            s_reg[idx].is_string = true;
        }
    }
    return 0;
}

void advapi32_init(void) {
    s_reg_count = 0;
    memset(s_reg, 0, sizeof(s_reg));
    win32_register_api("ADVAPI32.DLL", "RegOpenKeyExA", 0, adv_RegOpenKeyExA, 5);
    win32_register_api("ADVAPI32.DLL", "RegOpenKeyExW", 0, adv_RegOpenKeyExW, 5);
    win32_register_api("ADVAPI32.DLL", "RegCreateKeyW", 0, adv_RegCreateKeyW, 3);
    win32_register_api("ADVAPI32.DLL", "RegGetValueW", 0, adv_RegGetValueW, 7);
    win32_register_api("ADVAPI32.DLL", "RegCreateKeyExA", 0, adv_RegCreateKeyExA, 9);
    win32_register_api("ADVAPI32.DLL", "RegCreateKeyExW", 0, adv_RegCreateKeyExW, 9);
    win32_register_api("ADVAPI32.DLL", "RegQueryValueExA", 0, adv_RegQueryValueExA, 6);
    win32_register_api("ADVAPI32.DLL", "RegQueryValueExW", 0, adv_RegQueryValueExW, 6);
    win32_register_api("ADVAPI32.DLL", "RegSetValueExA", 0, adv_RegSetValueExA, 6);
    win32_register_api("ADVAPI32.DLL", "RegSetValueExW", 0, adv_RegSetValueExW, 6);
    win32_register_api("ADVAPI32.DLL", "RegCloseKey", 0, adv_RegCloseKey, 1);
}
