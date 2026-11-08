#include "sync.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#define SYNC_TABLE_SIZE 1024

typedef struct SRWNode {
    DWORD guest_ptr;
    pthread_rwlock_t rwlock;
    struct SRWNode* next;
} SRWNode;

typedef struct CVNode {
    DWORD guest_ptr;
    pthread_cond_t cond;
    struct CVNode* next;
} CVNode;

static SRWNode* s_srw_table[SYNC_TABLE_SIZE] = {0};
static CVNode*  s_cv_table[SYNC_TABLE_SIZE] = {0};
static pthread_mutex_t s_sync_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash_ptr(DWORD ptr) {
    return (ptr >> 2) % SYNC_TABLE_SIZE;
}

static pthread_rwlock_t* get_or_create_srw(PSRWLOCK lock) {
    if (!lock) return NULL;
    DWORD guest_ptr = lock->Ptr;
    if (guest_ptr == 0) {
        // Uninitialized lock address
        guest_ptr = (DWORD)(uintptr_t)lock;
    }

    uint32_t bucket = hash_ptr(guest_ptr);
    pthread_mutex_lock(&s_sync_mutex);

    SRWNode* curr = s_srw_table[bucket];
    while (curr) {
        if (curr->guest_ptr == guest_ptr) {
            pthread_mutex_unlock(&s_sync_mutex);
            return &curr->rwlock;
        }
        curr = curr->next;
    }

    // Allocate new node
    SRWNode* node = (SRWNode*)malloc(sizeof(SRWNode));
    node->guest_ptr = guest_ptr;
    pthread_rwlock_init(&node->rwlock, NULL);
    node->next = s_srw_table[bucket];
    s_srw_table[bucket] = node;

    pthread_mutex_unlock(&s_sync_mutex);
    return &node->rwlock;
}

static pthread_cond_t* get_or_create_cv(PCONDITION_VARIABLE cv) {
    if (!cv) return NULL;
    DWORD guest_ptr = cv->Ptr;
    if (guest_ptr == 0) {
        guest_ptr = (DWORD)(uintptr_t)cv;
    }

    uint32_t bucket = hash_ptr(guest_ptr);
    pthread_mutex_lock(&s_sync_mutex);

    CVNode* curr = s_cv_table[bucket];
    while (curr) {
        if (curr->guest_ptr == guest_ptr) {
            pthread_mutex_unlock(&s_sync_mutex);
            return &curr->cond;
        }
        curr = curr->next;
    }

    CVNode* node = (CVNode*)malloc(sizeof(CVNode));
    node->guest_ptr = guest_ptr;
    pthread_cond_init(&node->cond, NULL);
    node->next = s_cv_table[bucket];
    s_cv_table[bucket] = node;

    pthread_mutex_unlock(&s_sync_mutex);
    return &node->cond;
}

void win7_init_srwlock(PSRWLOCK lock) {
    if (lock) lock->Ptr = 0;
}

void win7_acquire_srwlock_exclusive(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (rw) pthread_rwlock_wrlock(rw);
}

void win7_release_srwlock_exclusive(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (rw) pthread_rwlock_unlock(rw);
}

void win7_acquire_srwlock_shared(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (rw) pthread_rwlock_rdlock(rw);
}

void win7_release_srwlock_shared(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (rw) pthread_rwlock_unlock(rw);
}

BOOL win7_try_acquire_srwlock_exclusive(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (!rw) return FALSE;
    return (pthread_rwlock_trywrlock(rw) == 0) ? TRUE : FALSE;
}

BOOL win7_try_acquire_srwlock_shared(PSRWLOCK lock) {
    pthread_rwlock_t* rw = get_or_create_srw(lock);
    if (!rw) return FALSE;
    return (pthread_rwlock_tryrdlock(rw) == 0) ? TRUE : FALSE;
}

void win7_init_condition_variable(PCONDITION_VARIABLE cv) {
    if (cv) cv->Ptr = 0;
}

void win7_wake_condition_variable(PCONDITION_VARIABLE cv) {
    pthread_cond_t* cond = get_or_create_cv(cv);
    if (cond) pthread_cond_signal(cond);
}

void win7_wake_all_condition_variable(PCONDITION_VARIABLE cv) {
    pthread_cond_t* cond = get_or_create_cv(cv);
    if (cond) pthread_cond_broadcast(cond);
}

BOOL win7_sleep_condition_variable_srw(PCONDITION_VARIABLE cv, PSRWLOCK srw, DWORD ms, ULONG flags) {
    pthread_cond_t* cond = get_or_create_cv(cv);
    pthread_rwlock_t* rw = get_or_create_srw(srw);
    if (!cond || !rw) return FALSE;

    // For single-process HLE, release lock, wait, reacquire
    pthread_rwlock_unlock(rw);

    if (ms == 0xFFFFFFFF) {
        // Infinite
        // In POSIX we need a mutex for pthread_cond_wait
        pthread_mutex_lock(&s_sync_mutex);
        pthread_cond_wait(cond, &s_sync_mutex);
        pthread_mutex_unlock(&s_sync_mutex);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_mutex_lock(&s_sync_mutex);
        pthread_cond_timedwait(cond, &s_sync_mutex, &ts);
        pthread_mutex_unlock(&s_sync_mutex);
    }

    if (flags & 1) { // CONDITION_VARIABLE_LOCKMODE_SHARED
        pthread_rwlock_rdlock(rw);
    } else {
        pthread_rwlock_wrlock(rw);
    }
    return TRUE;
}

// Windows 7 InitOnce implementation
#define INIT_ONCE_CHECK_ONLY        0x00000001
#define INIT_ONCE_ASYNC             0x00000002
#define INIT_ONCE_INIT_FAILED       0x00000004
#define INIT_ONCE_CTX_RESERVED_BITS 2

void win7_init_once_initialize(PINIT_ONCE init_once) {
    if (init_once) init_once->Ptr = 0;
}

BOOL win7_init_once_begin(PINIT_ONCE init_once, DWORD flags, BOOL* pending, LPVOID* context) {
    if (!init_once || !pending) return FALSE;

    pthread_mutex_lock(&s_sync_mutex);
    uintptr_t val = (uintptr_t)init_once->Ptr;

    if (val & 1) {
        // Completed
        *pending = FALSE;
        if (context) *context = (LPVOID)(val & ~((uintptr_t)3));
        pthread_mutex_unlock(&s_sync_mutex);
        return TRUE;
    }

    if (flags & INIT_ONCE_CHECK_ONLY) {
        *pending = TRUE;
        pthread_mutex_unlock(&s_sync_mutex);
        return FALSE;
    }

    // Mark as in-progress / pending
    *pending = TRUE;
    pthread_mutex_unlock(&s_sync_mutex);
    return TRUE;
}

BOOL win7_init_once_complete(PINIT_ONCE init_once, DWORD flags, LPVOID context) {
    if (!init_once) return FALSE;

    pthread_mutex_lock(&s_sync_mutex);
    if (flags & INIT_ONCE_INIT_FAILED) {
        init_once->Ptr = 0;
    } else {
        // Tag bit 0 as completed
        init_once->Ptr = (PVOID)(((uintptr_t)context & ~((uintptr_t)3)) | 1);
    }
    pthread_mutex_unlock(&s_sync_mutex);
    return TRUE;
}

// Windows 8/10 WaitOnAddress (Futex)
typedef struct WaitOnNode {
    DWORD addr;
    pthread_cond_t cond;
    struct WaitOnNode* next;
} WaitOnNode;

static WaitOnNode* s_waiton_head = NULL;

BOOL win10_wait_on_address(DWORD addr, DWORD cmp_addr, SIZE_T size, DWORD ms) {
    if (!addr || !cmp_addr) return FALSE;

    // Compare memory contents
    extern bool mem_read(DWORD vaddr, void* dst, size_t size);
    uint8_t current_val[8] = {0};
    uint8_t cmp_val[8] = {0};
    if (size > 8) size = 8;

    if (!mem_read(addr, current_val, size)) return FALSE;
    if (!mem_read(cmp_addr, cmp_val, size)) return FALSE;

    if (memcmp(current_val, cmp_val, size) != 0) {
        // Memory has changed, return immediately without waiting
        return TRUE;
    }

    pthread_mutex_lock(&s_sync_mutex);
    WaitOnNode* node = (WaitOnNode*)malloc(sizeof(WaitOnNode));
    node->addr = addr;
    pthread_cond_init(&node->cond, NULL);
    node->next = s_waiton_head;
    s_waiton_head = node;

    if (ms == 0xFFFFFFFF) {
        pthread_cond_wait(&node->cond, &s_sync_mutex);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&node->cond, &s_sync_mutex, &ts);
    }

    // Remove node
    WaitOnNode** curr = &s_waiton_head;
    while (*curr) {
        if (*curr == node) {
            *curr = node->next;
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_cond_destroy(&node->cond);
    free(node);

    pthread_mutex_unlock(&s_sync_mutex);
    return TRUE;
}

void win10_wake_by_address_single(DWORD addr) {
    pthread_mutex_lock(&s_sync_mutex);
    WaitOnNode* curr = s_waiton_head;
    while (curr) {
        if (curr->addr == addr) {
            pthread_cond_signal(&curr->cond);
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&s_sync_mutex);
}

void win10_wake_by_address_all(DWORD addr) {
    pthread_mutex_lock(&s_sync_mutex);
    WaitOnNode* curr = s_waiton_head;
    while (curr) {
        if (curr->addr == addr) {
            pthread_cond_broadcast(&curr->cond);
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&s_sync_mutex);
}
