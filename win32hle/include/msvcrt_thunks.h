#ifndef WIN32HLE_MSVCRT_THUNKS_H
#define WIN32HLE_MSVCRT_THUNKS_H

#include "types.h"
#include <stdbool.h>

void register_msvcrt_apis(void);
void msvcrt_init_cmdline(int argc, char** argv);
uint32_t msvcrt_get_iob_addr(void);

#endif // WIN32HLE_MSVCRT_THUNKS_H
