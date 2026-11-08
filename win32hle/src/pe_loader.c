#include "pe_loader.h"
#include "pe.h"
#include "memory.h"
#include "hle.h"
#include "apiset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int pe_load(const char* filepath, LoadedPE* out_pe) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        perror("Failed to open PE file");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t* raw_data = (uint8_t*)malloc(file_size);
    if (!raw_data) {
        fclose(fp);
        return -1;
    }

    if (fread(raw_data, 1, file_size, fp) != (size_t)file_size) {
        fclose(fp);
        free(raw_data);
        return -1;
    }
    fclose(fp);

    // 1. Verify DOS Header
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)raw_data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        fprintf(stderr, "Error: Invalid DOS signature (0x%04X)\n", dos->e_magic);
        free(raw_data);
        return -1;
    }

    // 2. Verify NT Headers
    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > (size_t)file_size) {
        fprintf(stderr, "Error: Invalid e_lfanew offset\n");
        free(raw_data);
        return -1;
    }

    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(raw_data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        fprintf(stderr, "Error: Invalid NT signature (0x%08X)\n", nt->Signature);
        free(raw_data);
        return -1;
    }

    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        fprintf(stderr, "Error: Unsupported PE Machine type (0x%04X, expected i386)\n", nt->FileHeader.Machine);
        free(raw_data);
        return -1;
    }

    DWORD pref_base = nt->OptionalHeader.ImageBase;
    DWORD img_size = nt->OptionalHeader.SizeOfImage;

    // 3. Allocate memory for image
    DWORD actual_base = mem_virtual_alloc(pref_base, img_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    DWORD reloc_delta = 0;
    if (actual_base != pref_base) {
        if (actual_base == 0) {
            // Try any free address
            actual_base = mem_virtual_alloc(0, img_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        }
        reloc_delta = actual_base - pref_base;
    }

    if (actual_base == 0) {
        fprintf(stderr, "Error: Failed to allocate virtual memory for PE image (size 0x%X)\n", img_size);
        free(raw_data);
        return -1;
    }

    // 4. Copy Headers
    DWORD headers_size = nt->OptionalHeader.SizeOfHeaders;
    mem_write(actual_base, raw_data, headers_size);

    // 5. Map Sections
    IMAGE_SECTION_HEADER* sections = (IMAGE_SECTION_HEADER*)((uint8_t*)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER* sec = &sections[i];
        DWORD sec_vaddr = actual_base + sec->VirtualAddress;
        DWORD sec_raw_size = sec->SizeOfRawData;
        DWORD sec_raw_ptr = sec->PointerToRawData;

        if (sec_raw_size > 0 && sec_raw_ptr < (DWORD)file_size) {
            if (sec_raw_ptr + sec_raw_size > (DWORD)file_size) {
                sec_raw_size = (DWORD)file_size - sec_raw_ptr;
            }
            mem_write(sec_vaddr, raw_data + sec_raw_ptr, sec_raw_size);
        }
    }

    // 6. Base Relocations if relocated
    if (reloc_delta != 0) {
        DWORD reloc_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        DWORD reloc_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

        if (reloc_rva && reloc_size) {
            DWORD curr_offset = 0;
            while (curr_offset < reloc_size) {
                IMAGE_BASE_RELOCATION* block = (IMAGE_BASE_RELOCATION*)mem_to_host(actual_base + reloc_rva + curr_offset);
                if (!block || block->SizeOfBlock == 0) break;

                DWORD page_rva = block->VirtualAddress;
                DWORD entries_count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* entries = (WORD*)((uint8_t*)block + sizeof(IMAGE_BASE_RELOCATION));

                for (DWORD e = 0; e < entries_count; e++) {
                    WORD type = entries[e] >> 12;
                    WORD offset = entries[e] & 0x0FFF;

                    if (type == IMAGE_REL_BASED_HIGHLOW) {
                        DWORD patch_addr = actual_base + page_rva + offset;
                        DWORD val = mem_read_u32(patch_addr);
                        mem_write_u32(patch_addr, val + reloc_delta);
                    }
                }
                curr_offset += block->SizeOfBlock;
            }
        }
    }

    // 7. Resolve Import Address Table (IAT) and hook HLE thunks
    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (import_rva) {
        IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)mem_to_host(actual_base + import_rva);
        while (desc && desc->Name != 0) {
            const char* dll_name = (const char*)mem_to_host(actual_base + desc->Name);
            const char* resolved_dll = apiset_resolve(dll_name);

            DWORD thunk_rva = desc->u.OriginalFirstThunk ? desc->u.OriginalFirstThunk : desc->FirstThunk;
            DWORD iat_rva = desc->FirstThunk;

            DWORD idx = 0;
            while (1) {
                DWORD thunk_val = mem_read_u32(actual_base + thunk_rva + idx * 4);
                if (thunk_val == 0) break;

                const char* func_name = NULL;
                uint32_t ordinal = 0;

                if (IMAGE_SNAP_BY_ORDINAL32(thunk_val)) {
                    ordinal = IMAGE_ORDINAL32(thunk_val);
                } else {
                    // Import by name
                    // thunk_val is RVA to IMAGE_IMPORT_BY_NAME (Hint: 2 bytes, Name: ASCII)
                    func_name = (const char*)mem_to_host(actual_base + thunk_val + 2);
                }

                uint32_t thunk_addr = hle_resolve(resolved_dll, func_name, ordinal);
                if (thunk_addr == 0) {
                    thunk_addr = hle_register_stub(resolved_dll, func_name, ordinal);
                }
                if (thunk_addr != 0) {
                    // Patch IAT with HLE synthetic thunk address
                    mem_write_u32(actual_base + iat_rva + idx * 4, thunk_addr);
                } else {
                    fprintf(stderr, "[PE Loader] WARNING: Failed to create thunk: %s -> %s (%s) ord %u\n",
                            dll_name, resolved_dll, func_name ? func_name : "<ordinal>", ordinal);
                }

                idx++;
            }
            desc++;
        }
    }

    out_pe->image_base = actual_base;
    out_pe->size_of_image = img_size;
    out_pe->entry_point = actual_base + nt->OptionalHeader.AddressOfEntryPoint;
    out_pe->stack_reserve = nt->OptionalHeader.SizeOfStackReserve;
    out_pe->stack_commit = nt->OptionalHeader.SizeOfStackCommit;
    out_pe->subsystem = nt->OptionalHeader.Subsystem;
    out_pe->is_relocated = (reloc_delta != 0);

    free(raw_data);
    return 0;
}
