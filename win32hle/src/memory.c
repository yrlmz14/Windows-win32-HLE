#include "memory.h"
#ifdef _WIN32
__declspec(dllimport) void* __stdcall VirtualAlloc(void* lpAddress, size_t dwSize, unsigned long flAllocationType, unsigned long flProtect);
__declspec(dllimport) int __stdcall VirtualFree(void* lpAddress, size_t dwSize, unsigned long dwFreeType);
__declspec(dllimport) int __stdcall VirtualProtect(void* lpAddress, size_t dwSize, unsigned long flNewProtect, unsigned long* lpflOldProtect);

#define WIN_PAGE_NOACCESS          0x01
#define WIN_PAGE_READONLY          0x02
#define WIN_PAGE_READWRITE         0x04
#define WIN_PAGE_EXECUTE           0x10
#define WIN_PAGE_EXECUTE_READ      0x20
#define WIN_PAGE_EXECUTE_READWRITE 0x40

#define WIN_MEM_COMMIT   0x00001000
#define WIN_MEM_RESERVE  0x00002000
#define WIN_MEM_DECOMMIT 0x00004000
#define WIN_MEM_RELEASE  0x00008000
#else
#include <sys/mman.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

MemoryManager g_mem = {0};

int mem_init(size_t max_commit_bytes) {
    if (g_mem.arena) return 0; // Already initialized

#ifdef _WIN32
    g_mem.arena = (uint8_t*)VirtualAlloc(NULL, WIN32_VIRTUAL_SPACE_SIZE, WIN_MEM_RESERVE, WIN_PAGE_NOACCESS);
    if (!g_mem.arena) {
        fprintf(stderr, "VirtualAlloc 2GB virtual arena failed\n");
        return -1;
    }
#else
    // Map 2GB virtual memory with MAP_NORESERVE (no physical RAM backing until committed)
    g_mem.arena = (uint8_t*)mmap(NULL, WIN32_VIRTUAL_SPACE_SIZE, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem.arena == MAP_FAILED) {
        perror("mmap 2GB virtual arena failed");
        g_mem.arena = NULL;
        return -1;
    }
#endif

    size_t num_pages = WIN32_VIRTUAL_SPACE_SIZE / PAGE_SIZE_4K; // 524,288 pages
    g_mem.page_state = (uint8_t*)calloc(num_pages, sizeof(uint8_t));
    if (!g_mem.page_state) {
#ifdef _WIN32
        VirtualFree(g_mem.arena, 0, WIN_MEM_RELEASE);
#else
        munmap(g_mem.arena, WIN32_VIRTUAL_SPACE_SIZE);
#endif
        g_mem.arena = NULL;
        return -1;
    }

    g_mem.current_committed = 0;
    g_mem.max_commit_limit = max_commit_bytes ? max_commit_bytes : DEFAULT_MAX_COMMIT_BYTES;
    g_mem.next_free_search = 0x00100000; // Start searching at 1MB

    return 0;
}

void mem_cleanup(void) {
    if (g_mem.arena) {
#ifdef _WIN32
        VirtualFree(g_mem.arena, 0, WIN_MEM_RELEASE);
#else
        munmap(g_mem.arena, WIN32_VIRTUAL_SPACE_SIZE);
#endif
        g_mem.arena = NULL;
    }
    if (g_mem.page_state) {
        free(g_mem.page_state);
        g_mem.page_state = NULL;
    }
    g_mem.current_committed = 0;
}

static int win_prot_to_host(DWORD flProtect) {
#ifdef _WIN32
    if ((flProtect & PAGE_READONLY)) return WIN_PAGE_READONLY;
    if ((flProtect & PAGE_READWRITE)) return WIN_PAGE_READWRITE;
    if ((flProtect & PAGE_EXECUTE)) return WIN_PAGE_EXECUTE;
    if ((flProtect & PAGE_EXECUTE_READ)) return WIN_PAGE_EXECUTE_READ;
    if ((flProtect & PAGE_EXECUTE_READWRITE)) return WIN_PAGE_EXECUTE_READWRITE;
    return WIN_PAGE_READWRITE;
#else
    int prot = PROT_NONE;
    if ((flProtect & PAGE_READONLY)) prot = PROT_READ;
    else if ((flProtect & PAGE_READWRITE)) prot = PROT_READ | PROT_WRITE;
    else if ((flProtect & PAGE_EXECUTE)) prot = PROT_READ | PROT_EXEC;
    else if ((flProtect & PAGE_EXECUTE_READ)) prot = PROT_READ | PROT_EXEC;
    else if ((flProtect & PAGE_EXECUTE_READWRITE)) prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    return prot;
#endif
}

static DWORD align_up(DWORD val, DWORD align) {
    return (val + align - 1) & ~(align - 1);
}

static DWORD align_down(DWORD val, DWORD align) {
    return val & ~(align - 1);
}

DWORD mem_virtual_alloc(DWORD lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
    if (dwSize == 0) return 0;
    if (!g_mem.arena) return 0;

    DWORD size_aligned = align_up(dwSize, PAGE_SIZE_4K);
    DWORD target_addr = lpAddress;

    // Search for free virtual address space if not specified
    if (target_addr == 0) {
        DWORD candidate = align_up(g_mem.next_free_search, ALLOCATION_GRANULARITY);
        DWORD pages_needed = size_aligned / PAGE_SIZE_4K;
        size_t total_pages = WIN32_VIRTUAL_SPACE_SIZE / PAGE_SIZE_4K;

        while (candidate + size_aligned < 0x7FFF0000) {
            DWORD start_page = candidate / PAGE_SIZE_4K;
            bool space_free = true;
            for (DWORD p = 0; p < pages_needed; p++) {
                if (start_page + p >= total_pages || g_mem.page_state[start_page + p] != 0) {
                    space_free = false;
                    candidate = align_up((start_page + p + 1) * PAGE_SIZE_4K, ALLOCATION_GRANULARITY);
                    break;
                }
            }
            if (space_free) {
                target_addr = candidate;
                g_mem.next_free_search = candidate + size_aligned;
                break;
            }
        }
    }

    if (target_addr == 0 || target_addr + size_aligned > WIN32_VIRTUAL_SPACE_SIZE) {
        return 0; // Out of virtual address space
    }

    DWORD start_page = target_addr / PAGE_SIZE_4K;
    DWORD num_pages = size_aligned / PAGE_SIZE_4K;

    // Check commit limit against 512MB RAM constraints
    if (flAllocationType & MEM_COMMIT) {
        size_t new_commit_bytes = 0;
        for (DWORD i = 0; i < num_pages; i++) {
            if ((g_mem.page_state[start_page + i] & 0x02) == 0) { // Not yet committed
                new_commit_bytes += PAGE_SIZE_4K;
            }
        }

        if (g_mem.current_committed + new_commit_bytes > g_mem.max_commit_limit) {
            fprintf(stderr, "[MEM] VirtualAlloc commit rejected: limit %zu MB exceeded (would reach %zu MB)\n",
                    g_mem.max_commit_limit / (1024 * 1024),
                    (g_mem.current_committed + new_commit_bytes) / (1024 * 1024));
            return 0; // ERROR_NOT_ENOUGH_MEMORY
        }

        int host_prot = win_prot_to_host(flProtect);
#ifdef _WIN32
        unsigned long old_prot;
        if (!VirtualAlloc(g_mem.arena + target_addr, size_aligned, WIN_MEM_COMMIT, host_prot)) {
            if (!VirtualProtect(g_mem.arena + target_addr, size_aligned, host_prot, &old_prot)) {
                fprintf(stderr, "VirtualAlloc/Protect commit failed\n");
                return 0;
            }
        }
#else
        if (mprotect(g_mem.arena + target_addr, size_aligned, host_prot) != 0) {
            perror("mprotect commit failed");
            return 0;
        }
#endif

        for (DWORD i = 0; i < num_pages; i++) {
            g_mem.page_state[start_page + i] = 0x03; // Reserved (0x01) | Committed (0x02)
        }
        g_mem.current_committed += new_commit_bytes;
    } else if (flAllocationType & MEM_RESERVE) {
        for (DWORD i = 0; i < num_pages; i++) {
            g_mem.page_state[start_page + i] = 0x01; // Reserved only
        }
    }

    return target_addr;
}

BOOL mem_virtual_free(DWORD lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    if (!g_mem.arena || lpAddress >= WIN32_VIRTUAL_SPACE_SIZE) return FALSE;

    DWORD addr = align_down(lpAddress, PAGE_SIZE_4K);
    DWORD size = (dwSize == 0) ? PAGE_SIZE_4K : align_up(dwSize, PAGE_SIZE_4K);
    DWORD start_page = addr / PAGE_SIZE_4K;
    DWORD num_pages = size / PAGE_SIZE_4K;

    if (dwFreeType & MEM_RELEASE) {
        // Uncommit and return pages to OS
#ifdef _WIN32
        VirtualFree(g_mem.arena + addr, size, MEM_DECOMMIT);
#else
        mprotect(g_mem.arena + addr, size, PROT_NONE);
        madvise(g_mem.arena + addr, size, MADV_DONTNEED);
#endif

        size_t released_bytes = 0;
        for (DWORD i = 0; i < num_pages; i++) {
            if (start_page + i < (WIN32_VIRTUAL_SPACE_SIZE / PAGE_SIZE_4K)) {
                if (g_mem.page_state[start_page + i] & 0x02) {
                    released_bytes += PAGE_SIZE_4K;
                }
                g_mem.page_state[start_page + i] = 0;
            }
        }
        if (g_mem.current_committed >= released_bytes) {
            g_mem.current_committed -= released_bytes;
        }
        return TRUE;
    } else if (dwFreeType & MEM_DECOMMIT) {
#ifdef _WIN32
        VirtualFree(g_mem.arena + addr, size, MEM_DECOMMIT);
#else
        mprotect(g_mem.arena + addr, size, PROT_NONE);
        madvise(g_mem.arena + addr, size, MADV_DONTNEED);
#endif

        size_t decommitted_bytes = 0;
        for (DWORD i = 0; i < num_pages; i++) {
            if (start_page + i < (WIN32_VIRTUAL_SPACE_SIZE / PAGE_SIZE_4K)) {
                if (g_mem.page_state[start_page + i] & 0x02) {
                    decommitted_bytes += PAGE_SIZE_4K;
                    g_mem.page_state[start_page + i] &= ~0x02; // Keep reserved
                }
            }
        }
        if (g_mem.current_committed >= decommitted_bytes) {
            g_mem.current_committed -= decommitted_bytes;
        }
        return TRUE;
    }

    return FALSE;
}

BOOL mem_virtual_protect(DWORD lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect) {
    if (!g_mem.arena || lpAddress >= WIN32_VIRTUAL_SPACE_SIZE) return FALSE;

    DWORD addr = align_down(lpAddress, PAGE_SIZE_4K);
    DWORD size = align_up(dwSize, PAGE_SIZE_4K);

    int host_prot = win_prot_to_host(flNewProtect);
#ifdef _WIN32
    unsigned long old_p = 0;
    if (!VirtualProtect(g_mem.arena + addr, size, host_prot, &old_p)) {
        return FALSE;
    }
#else
    if (mprotect(g_mem.arena + addr, size, host_prot) != 0) {
        return FALSE;
    }
#endif

    if (lpflOldProtect) {
        *lpflOldProtect = PAGE_EXECUTE_READWRITE;
    }
    return TRUE;
}

SIZE_T mem_virtual_query(DWORD lpAddress, MEMORY_BASIC_INFORMATION* lpBuffer, SIZE_T dwLength) {
    if (!lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION)) return 0;

    DWORD addr = align_down(lpAddress, PAGE_SIZE_4K);
    DWORD page = addr / PAGE_SIZE_4K;
    if (page >= (WIN32_VIRTUAL_SPACE_SIZE / PAGE_SIZE_4K)) return 0;

    uint8_t state = g_mem.page_state[page];
    lpBuffer->BaseAddress = addr;
    lpBuffer->AllocationBase = align_down(addr, ALLOCATION_GRANULARITY);
    lpBuffer->AllocationProtect = PAGE_READWRITE;
    lpBuffer->RegionSize = PAGE_SIZE_4K;

    if (state & 0x02) {
        lpBuffer->State = MEM_COMMIT;
        lpBuffer->Protect = PAGE_READWRITE;
    } else if (state & 0x01) {
        lpBuffer->State = MEM_RESERVE;
        lpBuffer->Protect = PAGE_NOACCESS;
    } else {
        lpBuffer->State = MEM_FREE;
        lpBuffer->Protect = PAGE_NOACCESS;
    }
    lpBuffer->Type = MEM_PRIVATE;

    return sizeof(MEMORY_BASIC_INFORMATION);
}

bool mem_read(DWORD vaddr, void* dst, size_t size) {
    if (vaddr + size > WIN32_VIRTUAL_SPACE_SIZE) return false;
    void* src = mem_to_host(vaddr);
    if (!src) return false;
    memcpy(dst, src, size);
    return true;
}

bool mem_write(DWORD vaddr, const void* src, size_t size) {
    if (vaddr + size > WIN32_VIRTUAL_SPACE_SIZE) return false;
    void* dst = mem_to_host(vaddr);
    if (!dst) return false;
    memcpy(dst, src, size);
    return true;
}

uint8_t mem_read_u8(DWORD vaddr) {
    uint8_t val = 0;
    mem_read(vaddr, &val, 1);
    return val;
}

uint16_t mem_read_u16(DWORD vaddr) {
    uint16_t val = 0;
    mem_read(vaddr, &val, 2);
    return val;
}

uint32_t mem_read_u32(DWORD vaddr) {
    uint32_t val = 0;
    mem_read(vaddr, &val, 4);
    return val;
}

void mem_write_u8(DWORD vaddr, uint8_t val) {
    mem_write(vaddr, &val, 1);
}

void mem_write_u16(DWORD vaddr, uint16_t val) {
    mem_write(vaddr, &val, 2);
}

void mem_write_u32(DWORD vaddr, uint32_t val) {
    mem_write(vaddr, &val, 4);
}
