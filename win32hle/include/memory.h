#ifndef WIN32HLE_MEMORY_H
#define WIN32HLE_MEMORY_H

#include "types.h"

#define PAGE_SIZE_4K 0x1000
#define ALLOCATION_GRANULARITY 0x10000 // 64KB
#define WIN32_VIRTUAL_SPACE_SIZE 0x80000000ULL // 2GB

// Default budget for 512MB RAM target hardware (e.g., max 384MB committed memory)
#define DEFAULT_MAX_COMMIT_BYTES (384 * 1024 * 1024)

#define MEM_COMMIT      0x00001000
#define MEM_RESERVE     0x00002000
#define MEM_DECOMMIT    0x00004000
#define MEM_RELEASE     0x00008000
#define MEM_FREE        0x00010000
#define MEM_PRIVATE     0x00020000
#define MEM_RESET       0x00080000
#define MEM_LARGE_PAGES 0x20000000

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

typedef struct {
    DWORD BaseAddress;
    DWORD AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;

typedef struct {
    uint8_t* arena;            // Pointer to host mmap of the 2GB virtual address space
    uint8_t* page_state;       // Page state table (512KB for 524,288 pages)
    size_t current_committed;  // Bytes committed
    size_t max_commit_limit;   // Maximum allowed commit bytes (for 512MB RAM constraint)
    DWORD next_free_search;    // Hint for VirtualAlloc address search
} MemoryManager;

extern MemoryManager g_mem;

int mem_init(size_t max_commit_bytes);
void mem_cleanup(void);

// Convert Win32 virtual address to host pointer
static inline void* mem_to_host(DWORD vaddr) {
    if (vaddr == 0 || vaddr >= WIN32_VIRTUAL_SPACE_SIZE || !g_mem.arena) return NULL;
    return (void*)(g_mem.arena + vaddr);
}

// Convert host pointer to Win32 virtual address
static inline DWORD host_to_mem(const void* ptr) {
    if (!g_mem.arena || (uint8_t*)ptr < g_mem.arena) return 0;
    size_t diff = (uint8_t*)ptr - g_mem.arena;
    if (diff >= WIN32_VIRTUAL_SPACE_SIZE) return 0;
    return (DWORD)diff;
}

DWORD mem_virtual_alloc(DWORD lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL  mem_virtual_free(DWORD lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL  mem_virtual_protect(DWORD lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
SIZE_T mem_virtual_query(DWORD lpAddress, MEMORY_BASIC_INFORMATION* lpBuffer, SIZE_T dwLength);

// Direct read/write helpers
bool mem_read(DWORD vaddr, void* dst, size_t size);
bool mem_write(DWORD vaddr, const void* src, size_t size);
uint8_t  mem_read_u8(DWORD vaddr);
uint16_t mem_read_u16(DWORD vaddr);
uint32_t mem_read_u32(DWORD vaddr);
void mem_write_u8(DWORD vaddr, uint8_t val);
void mem_write_u16(DWORD vaddr, uint16_t val);
void mem_write_u32(DWORD vaddr, uint32_t val);

#endif // WIN32HLE_MEMORY_H
