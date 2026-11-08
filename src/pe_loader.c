#include "pe_loader.h"
#include "win32/win32_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096U
#define PAGE_MASK 0xFFFU

void guest_mem_init(guest_memory_t *gm) {
    if (!gm) return;
    memset(gm->l1, 0, sizeof(gm->l1));
}

void guest_mem_free(guest_memory_t *gm) {
    if (!gm) return;
    for (int i = 0; i < 1024; i++) {
        if (gm->l1[i]) {
            for (int j = 0; j < 1024; j++) {
                if (gm->l1[i][j]) {
                    free(gm->l1[i][j]);
                    gm->l1[i][j] = NULL;
                }
            }
            free(gm->l1[i]);
            gm->l1[i] = NULL;
        }
    }
}

uint8_t *guest_alloc_page(guest_memory_t *gm, uint32_t va) {
    if (!gm) return NULL;
    uint32_t l1_idx = va >> 22;
    uint32_t l2_idx = (va >> 12) & 0x3FF;

    if (!gm->l1[l1_idx]) {
        gm->l1[l1_idx] = (uint8_t **)calloc(1024, sizeof(uint8_t *));
        if (!gm->l1[l1_idx]) return NULL;
    }

    if (!gm->l1[l1_idx][l2_idx]) {
        gm->l1[l1_idx][l2_idx] = (uint8_t *)calloc(PAGE_SIZE, 1);
        if (!gm->l1[l1_idx][l2_idx]) return NULL;
    }

    return gm->l1[l1_idx][l2_idx] + (va & PAGE_MASK);
}

uint8_t *guest_va_to_host(const guest_memory_t *gm, uint32_t va) {
    if (!gm) return NULL;
    uint32_t l1_idx = va >> 22;
    uint32_t l2_idx = (va >> 12) & 0x3FF;

    if (!gm->l1[l1_idx]) return NULL;
    uint8_t *page = gm->l1[l1_idx][l2_idx];
    if (!page) return NULL;
    return page + (va & PAGE_MASK);
}

bool guest_mem_commit(guest_memory_t *gm, uint32_t va, uint32_t size) {
    if (!gm || size == 0) return false;
    uint32_t start_page = va & ~PAGE_MASK;
    uint32_t end_page = (va + size + PAGE_MASK) & ~PAGE_MASK;

    for (uint32_t p = start_page; p < end_page; p += PAGE_SIZE) {
        if (!guest_alloc_page(gm, p)) return false;
    }
    return true;
}

bool guest_mem_decommit(guest_memory_t *gm, uint32_t va, uint32_t size) {
    if (!gm || size == 0) return false;
    uint32_t start_page = va & ~PAGE_MASK;
    uint32_t end_page = (va + size + PAGE_MASK) & ~PAGE_MASK;

    for (uint32_t p = start_page; p < end_page; p += PAGE_SIZE) {
        uint32_t l1_idx = p >> 22;
        uint32_t l2_idx = (p >> 12) & 0x3FF;
        if (gm->l1[l1_idx] && gm->l1[l1_idx][l2_idx]) {
            free(gm->l1[l1_idx][l2_idx]);
            gm->l1[l1_idx][l2_idx] = NULL;
        }
    }
    return true;
}

bool guest_mem_write(guest_memory_t *gm, uint32_t va, const void *src, size_t len) {
    if (!gm || !src) return false;
    const uint8_t *s = (const uint8_t *)src;
    size_t written = 0;
    while (written < len) {
        uint32_t cur_va = va + written;
        uint32_t page_off = cur_va & PAGE_MASK;
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - written) chunk = len - written;

        uint8_t *host_p = guest_alloc_page(gm, cur_va);
        if (!host_p) return false;
        memcpy(host_p, s + written, chunk);
        written += chunk;
    }
    return true;
}

bool guest_mem_read(const guest_memory_t *gm, uint32_t va, void *dst, size_t len) {
    if (!gm || !dst) return false;
    uint8_t *d = (uint8_t *)dst;
    size_t read_bytes = 0;
    while (read_bytes < len) {
        uint32_t cur_va = va + read_bytes;
        uint32_t page_off = cur_va & PAGE_MASK;
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - read_bytes) chunk = len - read_bytes;

        uint8_t *host_p = guest_va_to_host(gm, cur_va);
        if (!host_p) {
            memset(d + read_bytes, 0, chunk);
        } else {
            memcpy(d + read_bytes, host_p, chunk);
        }
        read_bytes += chunk;
    }
    return true;
}

uint8_t *x86_guest_va_to_host(const x86_context_t *ctx, uint32_t va) {
    if (!ctx || !ctx->user_data) return NULL;
    const win32_process_t *proc = (const win32_process_t *)ctx->user_data;
    return guest_va_to_host(&proc->mem, va);
}

bool pe_load_buffer(win32_process_t *proc, const uint8_t *buf, size_t buf_size) {
    if (!proc || !buf || buf_size < sizeof(pe_dos_header_t)) {
        LOGE("Invalid PE buffer or process");
        return false;
    }

    const pe_dos_header_t *dos = (const pe_dos_header_t *)buf;
    if (dos->e_magic != 0x5A4D) {
        LOGE("Missing MZ signature (0x%04X)", dos->e_magic);
        return false;
    }

    if (dos->e_lfanew + sizeof(uint32_t) + sizeof(pe_file_header_t) + sizeof(pe_optional_header32_t) > buf_size) {
        LOGE("Corrupted PE header offset");
        return false;
    }

    const uint8_t *nt_ptr = buf + dos->e_lfanew;
    uint32_t pe_sig = *(const uint32_t *)nt_ptr;
    if (pe_sig != 0x00004550) { /* "PE\0\0" */
        LOGE("Invalid PE signature (0x%08X)", (unsigned int)pe_sig);
        return false;
    }

    const pe_file_header_t *file_hdr = (const pe_file_header_t *)(nt_ptr + 4);
    if (file_hdr->Machine != 0x014C) { /* IMAGE_FILE_MACHINE_I386 */
        LOGE("Not an x86 i386 binary (Machine: 0x%04X)", file_hdr->Machine);
        return false;
    }

    const pe_optional_header32_t *opt = (const pe_optional_header32_t *)((const uint8_t *)file_hdr + sizeof(pe_file_header_t));
    if (opt->Magic != 0x10B) { /* PE32 */
        LOGE("Not a PE32 executable (Magic: 0x%04X)", opt->Magic);
        return false;
    }

    proc->image_base = opt->ImageBase;
    proc->entry_point = opt->ImageBase + opt->AddressOfEntryPoint;
    proc->size_of_image = opt->SizeOfImage;
    proc->is_gui = (opt->Subsystem == 2);
    proc->rsrc_rva = (opt->NumberOfRvaAndSizes > 2) ? opt->DataDirectory[2].VirtualAddress : 0;
    proc->rsrc_size = (opt->NumberOfRvaAndSizes > 2) ? opt->DataDirectory[2].Size : 0;

    LOGI("Loading PE32: Base=0x%08X, Entry=0x%08X, Size=%u KB, Subsystem=%s",
         proc->image_base, proc->entry_point, proc->size_of_image / 1024,
         proc->is_gui ? "GUI (Win32)" : "CUI (Console)");

    /* 1. Commit and copy Headers */
    size_t hdr_copy = opt->SizeOfHeaders < buf_size ? opt->SizeOfHeaders : buf_size;
    guest_mem_commit(&proc->mem, proc->image_base, hdr_copy);
    guest_mem_write(&proc->mem, proc->image_base, buf, hdr_copy);

    /* 2. Commit and map each Section */
    const pe_section_header_t *sec = (const pe_section_header_t *)((const uint8_t *)opt + file_hdr->SizeOfOptionalHeader);
    for (int i = 0; i < file_hdr->NumberOfSections; i++) {
        char sec_name[9] = {0};
        memcpy(sec_name, sec[i].Name, 8);
        uint32_t sec_va = proc->image_base + sec[i].VirtualAddress;
        uint32_t sec_vsize = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;

        LOGD("Section %s: VA=0x%08X, VSize=%u, RawOff=0x%X, RawSize=%u",
             sec_name, sec_va, sec_vsize, (unsigned int)sec[i].PointerToRawData, (unsigned int)sec[i].SizeOfRawData);

        guest_mem_commit(&proc->mem, sec_va, sec_vsize);

        if (sec[i].PointerToRawData < buf_size && sec[i].SizeOfRawData > 0) {
            size_t copy_len = sec[i].SizeOfRawData;
            if (sec[i].PointerToRawData + copy_len > buf_size) {
                copy_len = buf_size - sec[i].PointerToRawData;
            }
            guest_mem_write(&proc->mem, sec_va, buf + sec[i].PointerToRawData, copy_len);
        }
    }

    /* 3. Resolve Imports (IAT Hooking) */
    if (opt->NumberOfRvaAndSizes > 1 && opt->DataDirectory[1].VirtualAddress != 0) {
        uint32_t import_rva = opt->DataDirectory[1].VirtualAddress;
        uint32_t cur_desc_va = proc->image_base + import_rva;

        while (1) {
            pe_import_descriptor_t desc;
            if (!guest_mem_read(&proc->mem, cur_desc_va, &desc, sizeof(desc))) break;
            if (desc.Name == 0 && desc.FirstThunk == 0) break;

            char dll_name[128] = {0};
            for (int k = 0; k < 127; k++) {
                uint8_t ch = 0;
                if (!guest_mem_read(&proc->mem, proc->image_base + desc.Name + k, &ch, 1) || ch == 0) break;
                dll_name[k] = (char)ch;
            }
            if (dll_name[0] == '\0') strcpy(dll_name, "UNKNOWN.DLL");

            uint32_t thunk_rva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
            uint32_t iat_rva = desc.FirstThunk;

            uint32_t cur_thunk_va = proc->image_base + thunk_rva;
            uint32_t cur_iat_va   = proc->image_base + iat_rva;

            while (1) {
                uint32_t val = 0;
                if (!guest_mem_read(&proc->mem, cur_thunk_va, &val, 4) || val == 0) break;

                char func_name[128] = {0};
                uint16_t ordinal = 0;

                if (val & 0x80000000U) {
                    ordinal = val & 0xFFFF;
                } else {
                    /* Read hint name: 2 bytes hint + null-terminated string */
                    for (int k = 0; k < 127; k++) {
                        uint8_t ch = 0;
                        if (!guest_mem_read(&proc->mem, proc->image_base + val + 2 + k, &ch, 1) || ch == 0) break;
                        func_name[k] = (char)ch;
                    }
                }

                int api_id = win32_register_or_find_api(dll_name, func_name[0] ? func_name : NULL, ordinal);
                /* Write synthetic HLE call vector or global variable pointer into IAT */
                uint32_t vector = HLE_API_BASE | (uint32_t)(api_id & 0xFFFF);
                if (strcasecmp(func_name, "_adjust_fdiv") == 0) {
                    uint32_t s_fdiv_var_va = 0x003F3000;
                    guest_mem_commit(&proc->mem, s_fdiv_var_va, 4096);
                    uint32_t zero = 0;
                    guest_mem_write(&proc->mem, s_fdiv_var_va, &zero, 4);
                    vector = s_fdiv_var_va;
                }
                guest_mem_write(&proc->mem, cur_iat_va, &vector, 4);

                cur_thunk_va += 4;
                cur_iat_va += 4;
            }

            cur_desc_va += sizeof(pe_import_descriptor_t);
        }
    }

    /* 4. Setup Stack at 0x00100000 - 0x00300000 (2 MB) */
    proc->stack_base = 0x00100000;
    proc->stack_top  = 0x00300000;
    guest_mem_commit(&proc->mem, proc->stack_base, proc->stack_top - proc->stack_base);

    /* 5. Initialize CPU State */
    memset(&proc->cpu, 0, sizeof(proc->cpu));
    proc->cpu.user_data = proc;
    proc->cpu.va_to_host = x86_guest_va_to_host;
    proc->cpu.regs.eip = proc->entry_point;
    proc->cpu.regs.esp = proc->stack_top - 64;
    proc->cpu.regs.ebp = proc->cpu.regs.esp;
    proc->cpu.regs.eflags = 0x00000202; /* IF set, bit 1 set */
    proc->cpu.running = true;

    /* Push standard Windows WinMain / main arguments onto stack */
    uint32_t *stack_ptr = (uint32_t *)guest_va_to_host(&proc->mem, proc->cpu.regs.esp);
    if (stack_ptr) {
        stack_ptr[0] = 0xFFFFFFFF;        /* Return address: synthetic halt */
        stack_ptr[1] = proc->image_base;  /* hInstance */
        stack_ptr[2] = 0;                 /* hPrevInstance */
        stack_ptr[3] = proc->image_base;  /* lpCmdLine */
        stack_ptr[4] = 1;                 /* nCmdShow (SW_SHOWNORMAL) */
    }

    /* Setup Thread Information Block (TIB) at FS:[0] */
    proc->cpu.tib_data[0] = 0xFFFFFFFF;        /* ExceptionList */
    proc->cpu.tib_data[1] = proc->stack_top;   /* StackBase */
    proc->cpu.tib_data[2] = proc->stack_base;  /* StackLimit */
    proc->cpu.tib_data[6] = 0x7FFDF000;        /* Self pointer */
    proc->cpu.tib_data[9] = 0x1234;            /* Process ID */
    proc->cpu.tib_data[10] = 0x5678;           /* Thread ID */

    LOGI("PE32 successfully loaded and ready for execution!");
    return true;
}

bool pe_load_file(win32_process_t *proc, const char *filepath) {
    if (!proc || !filepath) return false;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        LOGE("Cannot open file: %s", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        LOGE("Invalid file size: %ld", sz);
        fclose(f);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        LOGE("Out of memory allocating %ld bytes", sz);
        fclose(f);
        return false;
    }

    if (fread(buf, 1, sz, f) != (size_t)sz) {
        LOGE("Failed reading file");
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    strncpy(proc->exe_path, filepath, sizeof(proc->exe_path) - 1);
    bool ok = pe_load_buffer(proc, buf, sz);
    free(buf);
    return ok;
}

uint32_t pe_find_resource_data(const win32_process_t *proc, uint32_t type_id, uint32_t name_id, uint32_t *out_size) {
    if (!proc || !proc->rsrc_rva) return 0;
    uint32_t rsrc_base_va = proc->image_base + proc->rsrc_rva;

    /* Level 1: Find type_id */
    uint16_t num_named = 0, num_id = 0;
    if (!guest_mem_read(&proc->mem, rsrc_base_va + 12, &num_named, 2)) return 0;
    if (!guest_mem_read(&proc->mem, rsrc_base_va + 14, &num_id, 2)) return 0;
    int total = num_named + num_id;

    uint32_t type_sub_off = 0;
    for (int i = 0; i < total; i++) {
        uint32_t eid = 0, eoff = 0;
        guest_mem_read(&proc->mem, rsrc_base_va + 16 + i * 8, &eid, 4);
        guest_mem_read(&proc->mem, rsrc_base_va + 20 + i * 8, &eoff, 4);
        if (eid == type_id && (eoff & 0x80000000U)) {
            type_sub_off = eoff & 0x7FFFFFFFU;
            break;
        }
    }
    if (!type_sub_off) return 0;

    /* Level 2: Find name_id */
    uint32_t name_dir_va = rsrc_base_va + type_sub_off;
    if (!guest_mem_read(&proc->mem, name_dir_va + 12, &num_named, 2)) return 0;
    if (!guest_mem_read(&proc->mem, name_dir_va + 14, &num_id, 2)) return 0;
    total = num_named + num_id;

    uint32_t name_sub_off = 0;
    for (int i = 0; i < total; i++) {
        uint32_t eid = 0, eoff = 0;
        guest_mem_read(&proc->mem, name_dir_va + 16 + i * 8, &eid, 4);
        guest_mem_read(&proc->mem, name_dir_va + 20 + i * 8, &eoff, 4);
        if (eid == name_id && (eoff & 0x80000000U)) {
            name_sub_off = eoff & 0x7FFFFFFFU;
            break;
        }
    }
    if (!name_sub_off) return 0;

    /* Level 3: First language entry */
    uint32_t lang_dir_va = rsrc_base_va + name_sub_off;
    uint32_t leaf_off = 0;
    guest_mem_read(&proc->mem, lang_dir_va + 20, &leaf_off, 4);
    if (leaf_off & 0x80000000U) return 0;

    /* Leaf: Read OffsetToData (RVA) and Size */
    uint32_t leaf_va = rsrc_base_va + leaf_off;
    uint32_t data_rva = 0, data_size = 0;
    guest_mem_read(&proc->mem, leaf_va, &data_rva, 4);
    guest_mem_read(&proc->mem, leaf_va + 4, &data_size, 4);

    if (out_size) *out_size = data_size;
    return proc->image_base + data_rva;
}

int pe_load_resource_string(const win32_process_t *proc, uint32_t string_id, uint16_t *out_str, int max_chars) {
    if (!proc || !proc->rsrc_rva || !out_str || max_chars <= 0) return 0;

    uint32_t rsrc_base_va = proc->image_base + proc->rsrc_rva;
    uint32_t target_block = (string_id >> 4) + 1;
    uint32_t str_index = string_id & 0x0F;

    /* Level 1: Find RT_STRING (type 6) */
    uint16_t num_named = 0, num_id = 0;
    if (!guest_mem_read(&proc->mem, rsrc_base_va + 12, &num_named, 2)) return 0;
    if (!guest_mem_read(&proc->mem, rsrc_base_va + 14, &num_id, 2)) return 0;
    int total_entries = num_named + num_id;

    uint32_t type_dir_off = 0;
    for (int i = 0; i < total_entries; i++) {
        uint32_t eid = 0, eoff = 0;
        guest_mem_read(&proc->mem, rsrc_base_va + 16 + i * 8, &eid, 4);
        guest_mem_read(&proc->mem, rsrc_base_va + 20 + i * 8, &eoff, 4);
        if (eid == 6 && (eoff & 0x80000000U)) { /* RT_STRING */
            type_dir_off = eoff & 0x7FFFFFFFU;
            break;
        }
    }
    if (!type_dir_off) return 0;

    /* Level 2: Find target_block */
    uint32_t block_dir_va = rsrc_base_va + type_dir_off;
    if (!guest_mem_read(&proc->mem, block_dir_va + 12, &num_named, 2)) return 0;
    if (!guest_mem_read(&proc->mem, block_dir_va + 14, &num_id, 2)) return 0;
    total_entries = num_named + num_id;

    uint32_t block_sub_off = 0;
    for (int i = 0; i < total_entries; i++) {
        uint32_t eid = 0, eoff = 0;
        guest_mem_read(&proc->mem, block_dir_va + 16 + i * 8, &eid, 4);
        guest_mem_read(&proc->mem, block_dir_va + 20 + i * 8, &eoff, 4);
        if (eid == target_block && (eoff & 0x80000000U)) {
            block_sub_off = eoff & 0x7FFFFFFFU;
            break;
        }
    }
    if (!block_sub_off) return 0;

    /* Level 3: First language entry */
    uint32_t lang_dir_va = rsrc_base_va + block_sub_off;
    uint32_t leaf_off = 0;
    guest_mem_read(&proc->mem, lang_dir_va + 20, &leaf_off, 4);
    if (leaf_off & 0x80000000U) return 0;

    /* Leaf: Read OffsetToData (RVA) and Size */
    uint32_t data_leaf_va = rsrc_base_va + leaf_off;
    uint32_t data_rva = 0, data_size = 0;
    guest_mem_read(&proc->mem, data_leaf_va, &data_rva, 4);
    guest_mem_read(&proc->mem, data_leaf_va + 4, &data_size, 4);

    uint32_t cur_va = proc->image_base + data_rva;
    for (uint32_t idx = 0; idx < 16; idx++) {
        uint16_t len = 0;
        if (!guest_mem_read(&proc->mem, cur_va, &len, 2)) return 0;
        cur_va += 2;
        if (idx == str_index) {
            int to_copy = len < max_chars - 1 ? len : max_chars - 1;
            if (to_copy > 0) {
                guest_mem_read(&proc->mem, cur_va, out_str, to_copy * 2);
            }
            out_str[to_copy] = 0;
            return to_copy;
        }
        cur_va += len * 2;
    }

    return 0;
}

#pragma pack(push, 1)
typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
} pe_resource_dir_t;

typedef struct {
    uint32_t Id;
    uint32_t OffsetToData;
} pe_resource_entry_t;

typedef struct {
    uint32_t OffsetToData;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} pe_resource_data_entry_t;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} pe_icon_bmp_hdr_t;
#pragma pack(pop)

bool pe_extract_icon_file(const char *filepath, int icon_index, uint32_t *out_argb32) {
    if (!filepath || !out_argb32) return false;

    char host_path[256];
    win32_translate_path(filepath, host_path, sizeof(host_path));

    FILE *f = fopen(host_path, "rb");
    if (!f) f = fopen(filepath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x200) { fclose(f); return false; }

    uint8_t *buf = (uint8_t *)malloc(fsz);
    if (!buf || fread(buf, 1, fsz, f) != (size_t)fsz) {
        if (buf) free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    pe_dos_header_t *dos = (pe_dos_header_t *)buf;
    if (dos->e_magic != 0x5A4D || dos->e_lfanew >= (uint32_t)fsz - 0x100) {
        free(buf); return false;
    }

    uint8_t *pe = buf + dos->e_lfanew;
    if (*(uint32_t *)pe != 0x00004550) { free(buf); return false; }

    uint16_t num_sections = *(uint16_t *)(pe + 6);
    uint16_t opt_hdr_sz = *(uint16_t *)(pe + 20);
    uint8_t *opt = pe + 24;

    uint32_t rsrc_rva = *(uint32_t *)(opt + 96 + 16);
    uint32_t rsrc_sz  = *(uint32_t *)(opt + 96 + 20);
    if (!rsrc_rva || !rsrc_sz) { free(buf); return false; }

    uint8_t *sec_hdrs = opt + opt_hdr_sz;
    uint32_t rsrc_file_off = 0;
    for (int i = 0; i < num_sections; i++) {
        uint8_t *sec = sec_hdrs + (i * 40);
        uint32_t vaddr = *(uint32_t *)(sec + 12);
        uint32_t raw_ptr = *(uint32_t *)(sec + 20);
        if (vaddr == rsrc_rva) {
            rsrc_file_off = raw_ptr;
            break;
        }
    }
    if (!rsrc_file_off || rsrc_file_off >= (uint32_t)fsz) { free(buf); return false; }

    uint8_t *rsrc_base = buf + rsrc_file_off;

    /* Root directory -> find RT_ICON (type 3) */
    pe_resource_dir_t *root_dir = (pe_resource_dir_t *)rsrc_base;
    int root_entries = root_dir->NumberOfNamedEntries + root_dir->NumberOfIdEntries;
    pe_resource_entry_t *root_table = (pe_resource_entry_t *)(rsrc_base + sizeof(pe_resource_dir_t));

    uint32_t type3_off = 0;
    for (int i = 0; i < root_entries; i++) {
        if (root_table[i].Id == 3 /* RT_ICON */ && (root_table[i].OffsetToData & 0x80000000)) {
            type3_off = root_table[i].OffsetToData & 0x7FFFFFFF;
            break;
        }
    }
    if (!type3_off) { free(buf); return false; }

    /* Name directory (all icons of type 3) */
    pe_resource_dir_t *name_dir = (pe_resource_dir_t *)(rsrc_base + type3_off);
    int icon_count = name_dir->NumberOfNamedEntries + name_dir->NumberOfIdEntries;
    pe_resource_entry_t *name_table = (pe_resource_entry_t *)(rsrc_base + type3_off + sizeof(pe_resource_dir_t));

    uint8_t *best_icon_data = NULL;
    int cur_idx = 0;

    for (int i = 0; i < icon_count; i++) {
        uint32_t lang_off = name_table[i].OffsetToData & 0x7FFFFFFF;
        pe_resource_entry_t *lang_table = (pe_resource_entry_t *)(rsrc_base + lang_off + sizeof(pe_resource_dir_t));

        uint32_t data_entry_off = lang_table[0].OffsetToData;
        pe_resource_data_entry_t *data_entry = (pe_resource_data_entry_t *)(rsrc_base + data_entry_off);

        uint32_t icon_file_off = (data_entry->OffsetToData - rsrc_rva) + rsrc_file_off;
        if (icon_file_off >= (uint32_t)fsz) continue;

        uint8_t *raw_dib = buf + icon_file_off;
        pe_icon_bmp_hdr_t *bih = (pe_icon_bmp_hdr_t *)raw_dib;

        if (icon_index > 0 && cur_idx == icon_index) {
            best_icon_data = raw_dib;
            break;
        }

        if (bih->biWidth == 32 && (bih->biHeight == 64 || bih->biHeight == 32)) {
            best_icon_data = raw_dib;
            if (bih->biBitCount == 32) break; /* Ideal 32bpp match */
        } else if (!best_icon_data && bih->biWidth >= 16) {
            best_icon_data = raw_dib;
        }
        cur_idx++;
    }

    if (!best_icon_data) { free(buf); return false; }

    /* Parse DIB */
    pe_icon_bmp_hdr_t *bih = (pe_icon_bmp_hdr_t *)best_icon_data;
    int w = bih->biWidth;
    int h = bih->biHeight / 2; /* In icon DIB, height = XOR height + AND height */
    int bpp = bih->biBitCount;

    uint8_t *palette = best_icon_data + bih->biSize;
    int colors = (bpp <= 8) ? (1 << bpp) : 0;
    if (bih->biClrUsed > 0 && bih->biClrUsed < (uint32_t)colors) colors = bih->biClrUsed;

    int xor_stride = ((w * bpp + 31) / 32) * 4;
    int and_stride = ((w + 31) / 32) * 4;

    uint8_t *xor_bits = palette + (colors * 4);
    uint8_t *and_bits = xor_bits + (xor_stride * h);

    /* Convert bottom-up DIB to 32x32 top-down ARGB32 */
    for (int y = 0; y < 32; y++) {
        int src_y = (y * h) / 32;
        int inv_y = h - 1 - src_y;

        uint8_t *xor_row = xor_bits + (inv_y * xor_stride);
        uint8_t *and_row = and_bits + (inv_y * and_stride);

        for (int x = 0; x < 32; x++) {
            int src_x = (x * w) / 32;
            bool is_mask_transparent = (and_row[src_x / 8] & (0x80 >> (src_x % 8))) != 0;

            uint32_t argb = 0;
            if (bpp == 32) {
                uint32_t val = *(uint32_t *)(xor_row + (src_x * 4));
                uint8_t a = (val >> 24) & 0xFF;
                if (a == 0 && !is_mask_transparent) a = 0xFF;
                if (is_mask_transparent) a = 0;
                argb = (a << 24) | (val & 0x00FFFFFF);
            } else if (bpp == 8) {
                uint8_t idx = xor_row[src_x];
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                uint8_t a = is_mask_transparent ? 0x00 : 0xFF;
                argb = (a << 24) | (r << 16) | (g << 8) | b;
            } else if (bpp == 4) {
                uint8_t byte_val = xor_row[src_x / 2];
                uint8_t idx = (src_x % 2 == 0) ? (byte_val >> 4) : (byte_val & 0x0F);
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                uint8_t a = is_mask_transparent ? 0x00 : 0xFF;
                argb = (a << 24) | (r << 16) | (g << 8) | b;
            } else if (bpp == 1) {
                uint8_t byte_val = xor_row[src_x / 8];
                uint8_t idx = (byte_val & (0x80 >> (src_x % 8))) ? 1 : 0;
                uint8_t b = palette[idx * 4 + 0];
                uint8_t g = palette[idx * 4 + 1];
                uint8_t r = palette[idx * 4 + 2];
                uint8_t a = is_mask_transparent ? 0x00 : 0xFF;
                argb = (a << 24) | (r << 16) | (g << 8) | b;
            }

            out_argb32[y * 32 + x] = argb;
        }
    }

    free(buf);
    return true;
}
