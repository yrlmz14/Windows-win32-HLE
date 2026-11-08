#ifndef KERNEL32_H
#define KERNEL32_H

#include "../common.h"
#include "../pe_loader.h"
#include "win32_types.h"

void kernel32_init(void);
uint32_t k32_HeapAlloc(win32_process_t *proc, uint32_t *args);

#endif /* KERNEL32_H */
