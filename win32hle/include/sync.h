#ifndef WIN32HLE_SYNC_H
#define WIN32HLE_SYNC_H

#include "types.h"
#include <pthread.h>

// Fast, low-memory implementation of Windows 7 SRWLock
// Windows SRWLock is 4 bytes. We maintain an internal pool/hash table mapping
// the guest SRWLOCK pointer to POSIX pthread_rwlock_t.
void win7_init_srwlock(PSRWLOCK lock);
void win7_acquire_srwlock_exclusive(PSRWLOCK lock);
void win7_release_srwlock_exclusive(PSRWLOCK lock);
void win7_acquire_srwlock_shared(PSRWLOCK lock);
void win7_release_srwlock_shared(PSRWLOCK lock);
BOOL win7_try_acquire_srwlock_exclusive(PSRWLOCK lock);
BOOL win7_try_acquire_srwlock_shared(PSRWLOCK lock);

// Windows 7 Condition Variables
void win7_init_condition_variable(PCONDITION_VARIABLE cv);
BOOL win7_sleep_condition_variable_cs(PCONDITION_VARIABLE cv, void* cs, DWORD ms);
BOOL win7_sleep_condition_variable_srw(PCONDITION_VARIABLE cv, PSRWLOCK srw, DWORD ms, ULONG flags);
void win7_wake_condition_variable(PCONDITION_VARIABLE cv);
void win7_wake_all_condition_variable(PCONDITION_VARIABLE cv);

// Windows 7 InitOnce
void win7_init_once_initialize(PINIT_ONCE init_once);
BOOL win7_init_once_begin(PINIT_ONCE init_once, DWORD flags, BOOL* pending, LPVOID* context);
BOOL win7_init_once_complete(PINIT_ONCE init_once, DWORD flags, LPVOID context);

// Windows 8/10 WaitOnAddress (Futex)
BOOL win10_wait_on_address(DWORD addr, DWORD cmp_addr, SIZE_T size, DWORD ms);
void win10_wake_by_address_single(DWORD addr);
void win10_wake_by_address_all(DWORD addr);

#endif // WIN32HLE_SYNC_H
