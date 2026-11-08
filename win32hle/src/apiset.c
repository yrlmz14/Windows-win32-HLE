#include "apiset.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>

typedef struct {
    const char* prefix;
    const char* target;
} ApiSetMapping;

static const ApiSetMapping s_mappings[] = {
    // Windows CRT (UCRT)
    { "api-ms-win-crt-",             "ucrtbase.dll" },
    { "ucrtbased.dll",               "ucrtbase.dll" },

    // Cryptography (Windows 7/10)
    { "api-ms-win-core-random-",     "bcrypt.dll" },
    { "bcryptprimitives.dll",        "bcrypt.dll" },

    // COM, WinRT, and Registry subsystems
    { "api-ms-win-core-com-",        "ole32.dll" },
    { "api-ms-win-core-winrt-",      "ole32.dll" },
    { "combase.dll",                 "ole32.dll" },
    { "api-ms-win-core-registry-",   "advapi32.dll" },
    { "api-ms-win-core-shlwapi-",    "shlwapi.dll" },

    // Core Windows subsystems
    { "api-ms-win-core-",            "kernel32.dll" },
    { "api-ms-win-security-",        "advapi32.dll" },
    { "api-ms-win-service-",         "advapi32.dll" },
    { "api-ms-win-eventing-",        "advapi32.dll" },
    { "api-ms-win-devices-",         "kernel32.dll" },
    { "api-ms-win-shcore-",          "shcore.dll" },
    { "api-ms-win-shell-",           "shell32.dll" },
    { "api-ms-win-gdi-",             "gdi32.dll" },
    { "api-ms-win-dx-",              "d3d11.dll" },
    { "ext-ms-win-ntuser-",          "user32.dll" },
    { "ext-ms-win-gdi-",             "gdi32.dll" },
    { "ext-ms-win-kernel32-",        "kernel32.dll" },
    { "ext-ms-win-advapi32-",        "advapi32.dll" },
    { "ext-ms-",                     "kernel32.dll" },

    // Modern split DLLs
    { "kernelbase.dll",              "kernel32.dll" },
    { "api-ms-win-downlevel-kernel32-", "kernel32.dll" },
    { "api-ms-win-downlevel-advapi32-", "advapi32.dll" },
    { "api-ms-win-downlevel-user32-",   "user32.dll" },
    { "api-ms-win-downlevel-shlwapi-",  "shlwapi.dll" },
    { NULL, NULL }
};

bool apiset_is_apiset(const char* dll_name) {
    if (!dll_name) return false;
    return (strncasecmp(dll_name, "api-ms-win-", 11) == 0 ||
            strncasecmp(dll_name, "ext-ms-", 7) == 0 ||
            strcasecmp(dll_name, "kernelbase.dll") == 0);
}

const char* apiset_resolve(const char* dll_name) {
    if (!dll_name) return NULL;

    for (int i = 0; s_mappings[i].prefix != NULL; i++) {
        size_t len = strlen(s_mappings[i].prefix);
        if (strncasecmp(dll_name, s_mappings[i].prefix, len) == 0) {
            return s_mappings[i].target;
        }
    }

    return dll_name; // Fallback to itself if not matched
}
