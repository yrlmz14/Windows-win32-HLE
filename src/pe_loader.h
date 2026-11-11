#ifndef PE_LOADER_H
#define PE_LOADER_H

#include "common.h"
#include "x86_core.h"
#include <stdio.h>

#define MAX_PE_SECTIONS 32

#pragma pack(push, 1)

typedef struct {
    uint16_t e_magic;    // 0x5A4D ("MZ")
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;   // Offset to NT Header
} pe_dos_header_t;

typedef struct {
    uint16_t Machine;               // 0x014C for i386
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} pe_file_header_t;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} pe_data_dir_t;

typedef struct {
    uint16_t Magic;                 // 0x10B (PE32)
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;             // 2 = GUI, 3 = CUI (Console)
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    pe_data_dir_t DataDirectory[16];
} pe_optional_header32_t;

typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} pe_section_header_t;

typedef struct {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} pe_import_descriptor_t;

typedef struct {
    uint32_t Attributes;
    uint32_t DllNameRVA;
    uint32_t ModuleHandleRVA;
    uint32_t ImportAddressTableRVA;
    uint32_t ImportNameTableRVA;
    uint32_t BoundImportAddressTableRVA;
    uint32_t UnloadInformationTableRVA;
    uint32_t TimeDateStamp;
} pe_delay_descriptor_t;

#pragma pack(pop)

/* Memory Page Directory structure (2-level sparse virtual memory: 4 GB address space) */
typedef struct {
    uint8_t **l1[1024]; /* 1024 L1 entries pointing to L2 page tables */
} guest_memory_t;

typedef struct {
    guest_memory_t mem;
    x86_context_t  cpu;
    uint32_t       image_base;
    uint32_t       entry_point;
    uint32_t       size_of_image;
    uint32_t       rsrc_rva;
    uint32_t       rsrc_size;
    uint32_t       stack_top;
    uint32_t       stack_base;
    uint32_t       heap_current;
    char           exe_path[256];
    char           cmd_line[256];
    bool           is_gui;
} win32_process_t;

/* Lifecycle & Memory Management */
void guest_mem_init(guest_memory_t *gm);
void guest_mem_free(guest_memory_t *gm);
uint8_t *guest_va_to_host(const guest_memory_t *gm, uint32_t va);
uint8_t *guest_alloc_page(guest_memory_t *gm, uint32_t va);
bool guest_mem_commit(guest_memory_t *gm, uint32_t va, uint32_t size);
bool guest_mem_decommit(guest_memory_t *gm, uint32_t va, uint32_t size);
bool guest_mem_write(guest_memory_t *gm, uint32_t va, const void *src, size_t len);
bool guest_mem_read(const guest_memory_t *gm, uint32_t va, void *dst, size_t len);

/* PE Loader APIs */
bool pe_load_file(win32_process_t *proc, const char *filepath);
bool pe_load_buffer(win32_process_t *proc, const uint8_t *buf, size_t buf_size);
int pe_load_resource_string(const win32_process_t *proc, uint32_t string_id, uint16_t *out_str, int max_chars);
uint32_t pe_find_resource_data(const win32_process_t *proc, uint32_t type_id, uint32_t name_id, uint32_t *out_size);
bool pe_extract_icon_file(const char *filepath, int icon_index, uint32_t *out_argb32);

/* Memory Read/Write Helpers for CPU */
uint8_t *x86_guest_va_to_host(const x86_context_t *ctx, uint32_t va);

#endif /* PE_LOADER_H */
