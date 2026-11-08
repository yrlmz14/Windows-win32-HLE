#ifndef WIN32HLE_HLE_H
#define WIN32HLE_HLE_H

#include "types.h"
#include <stdbool.h>

#define MAX_HLE_FUNCTIONS 2048
#define THUNK_BASE_ADDRESS 0x70000000
#define THUNK_STRIDE 8

// Forward declare CPU
struct CPUState;

typedef uint32_t (*HLEHandler)(struct CPUState* cpu, uint32_t* args);

typedef struct {
    const char* module_name;
    const char* function_name;
    uint32_t    ordinal;
    HLEHandler  handler;
    uint8_t     num_args;       // Number of 32-bit stack arguments
    bool        is_cdecl;       // false = stdcall (callee cleans), true = cdecl (caller cleans)
    uint32_t    thunk_address;  // Synthetic address in Win32 space
} HLEFunction;

typedef struct {
    HLEFunction functions[MAX_HLE_FUNCTIONS];
    uint32_t    count;
    bool        trace_calls;
    DWORD       last_error;
} HLEContext;

extern HLEContext g_hle;

void hle_init(bool trace_calls);
int  hle_register(const char* module, const char* name, uint32_t ordinal,
                  HLEHandler handler, uint8_t num_args, bool is_cdecl);

uint32_t hle_resolve(const char* module, const char* name, uint32_t ordinal);
uint32_t hle_register_stub(const char* module, const char* name, uint32_t ordinal);
HLEFunction* hle_get_by_id(uint32_t id);
HLEFunction* hle_get_by_address(uint32_t address);

void hle_set_last_error(DWORD error);
DWORD hle_get_last_error(void);

// Win32 Handle Table
HANDLE handle_alloc(void* native_ptr, uint32_t type);
void*  handle_get(HANDLE h, uint32_t type);
void   handle_free(HANDLE h);

#define HANDLE_TYPE_FILE      1
#define HANDLE_TYPE_MUTEX     2
#define HANDLE_TYPE_EVENT     3
#define HANDLE_TYPE_HEAP      4
#define HANDLE_TYPE_THREAD    5
#define HANDLE_TYPE_PROCESS   6
#define HANDLE_TYPE_SEMAPHORE 7
#define HANDLE_TYPE_FIND      8

#endif // WIN32HLE_HLE_H
