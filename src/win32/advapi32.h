#ifndef ADVAPI32_H
#define ADVAPI32_H

#include "../common.h"
#include "../pe_loader.h"
#include "win32_types.h"

#define HKEY_CLASSES_ROOT   ((H_HKEY)0x80000000)
#define HKEY_CURRENT_USER   ((H_HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE  ((H_HKEY)0x80000002)
#define HKEY_USERS          ((H_HKEY)0x80000003)

void advapi32_init(void);

#endif /* ADVAPI32_H */
