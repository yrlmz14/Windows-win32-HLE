#include "win32_dispatch.h"
#include "kernel32.h"
#include "user32.h"
#include "gdi32.h"
#include "advapi32.h"
#include "comdlg32.h"
#include "msvcrt.h"
#include "cards.h"
#include <strings.h>

#define MAX_HLE_APIS 512

static hle_api_entry_t s_api_table[MAX_HLE_APIS];
static int s_api_count = 0;

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

    /* 2. Fallback: dynamically register stub */
    if (s_api_count < MAX_HLE_APIS) {
        int idx = s_api_count++;
        s_api_table[idx].dll_name = strdup(dll_name);
        s_api_table[idx].func_name = func_name ? strdup(func_name) : "(by-ordinal)";
        s_api_table[idx].ordinal = ordinal;
        s_api_table[idx].handler = fallback_unimplemented_api;
        s_api_table[idx].arg_count = 0;
        LOGW("Registered placeholder stub for [%s]!%s (id=%d)", dll_name, s_api_table[idx].func_name, idx);
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
               strcmp(entry->func_name, "TranslateAcceleratorW") != 0 &&
               strcmp(entry->func_name, "TranslateMessage") != 0 &&
               strcmp(entry->func_name, "DispatchMessageW") != 0) {
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

    /* Stack unwinding: msvcrt is __cdecl (caller pops args), Windows DLLs are __stdcall */
    bool is_cdecl = (strcasecmp(entry->dll_name, "msvcrt.dll") == 0 ||
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
    user32_init();
    gdi32_init();
    advapi32_init();
    comdlg32_init();
    msvcrt_init();
    cards_init();
    LOGI("Win32 Subsystem initialized with %d native API handlers", s_api_count);
}
