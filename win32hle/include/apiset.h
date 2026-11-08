#ifndef WIN32HLE_APISET_H
#define WIN32HLE_APISET_H

#include <stdbool.h>

// Resolve virtual DLL names like "api-ms-win-core-synch-l1-1-0.dll" to canonical DLL name (e.g. "kernel32.dll")
const char* apiset_resolve(const char* dll_name);

// Check if DLL name belongs to Windows 7 / 10 ApiSet
bool apiset_is_apiset(const char* dll_name);

#endif // WIN32HLE_APISET_H
