#ifndef WIN32_DISPATCH_H
#define WIN32_DISPATCH_H

#include "../common.h"
#include "../pe_loader.h"

#define HLE_API_BASE 0xFF000000U
#define HLE_API_MASK 0xFF000000U

typedef uint32_t (*hle_api_fn)(win32_process_t *proc, uint32_t *stack_args);

typedef struct {
    const char *dll_name;
    const char *func_name;
    uint16_t    ordinal;
    hle_api_fn  handler;
    uint8_t     arg_count; /* Number of 32-bit parameters pushed to stack */
} hle_api_entry_t;

/* Registry lookup */
void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);
int  win32_register_or_find_api(const char *dll_name, const char *func_name, uint16_t ordinal);
bool win32_dispatch_api(win32_process_t *proc, uint32_t api_id);

/* Initialization of all Win32 API modules */
void win32_init_subsystem(win32_process_t *proc);

#endif /* WIN32_DISPATCH_H */
