#ifndef WIN32HLE_PE_LOADER_H
#define WIN32HLE_PE_LOADER_H

#include "types.h"
#include "pe.h"

typedef struct {
    DWORD image_base;
    DWORD size_of_image;
    DWORD entry_point;
    DWORD stack_reserve;
    DWORD stack_commit;
    WORD  subsystem;
    bool  is_relocated;
} LoadedPE;

int pe_load(const char* filepath, LoadedPE* out_pe);

#endif // WIN32HLE_PE_LOADER_H
