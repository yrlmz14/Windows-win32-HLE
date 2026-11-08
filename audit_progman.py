import struct, glob

data = open("C:/tiny-xtensa-compiler/samples/progman.exe", "rb").read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
opt_hdr_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
opt_hdr_off = e_lfanew + 24
import_rva = struct.unpack_from("<I", data, opt_hdr_off + 104)[0]

sections_off = opt_hdr_off + opt_hdr_size
sections = []
for i in range(num_sections):
    sec = data[sections_off + i*40 : sections_off + (i+1)*40]
    name = sec[:8].rstrip(b"\x00").decode("ascii", errors="ignore")
    vsize, va, rsize, roff = struct.unpack_from("<IIII", sec, 8)
    sections.append((name, va, vsize, roff, rsize))

def rva2off(rva):
    for name, va, vsize, roff, rsize in sections:
        if va <= rva < va + vsize:
            return roff + (rva - va)
    return None

imp_off = rva2off(import_rva)
all_imports = {}
while imp_off and imp_off + 20 <= len(data):
    ilt_rva, ts, fc, name_rva, iat_rva = struct.unpack_from("<IIIII", data, imp_off)
    if ilt_rva == 0 and name_rva == 0:
        break
    dll_off = rva2off(name_rva)
    dll_name = data[dll_off:data.find(b"\x00", dll_off)].decode("ascii").upper()
    all_imports[dll_name] = []
    
    table_rva = ilt_rva if ilt_rva else iat_rva
    table_off = rva2off(table_rva)
    while True:
        entry = struct.unpack_from("<I", data, table_off)[0]
        if entry == 0:
            break
        if entry & 0x80000000:
            all_imports[dll_name].append(f"Ordinal #{entry & 0xFFFF}")
        else:
            name_off = rva2off(entry)
            fname = data[name_off+2:data.find(b"\x00", name_off+2)].decode("ascii")
            all_imports[dll_name].append(fname)
        table_off += 4
    imp_off += 20

hle_code = ""
for f in glob.glob("C:/kindle-win32/src/win32/*.c"):
    hle_code += open(f, encoding="utf-8", errors="ignore").read() + "\n"

missing = {}
resolved = 0
total = 0
for dll, funcs in all_imports.items():
    missing[dll] = []
    for fn in funcs:
        total += 1
        if f'"{fn}"' in hle_code:
            resolved += 1
        else:
            missing[dll].append(fn)

print(f"Total progman imports: {total}")
print(f"Already in kindle-win32: {resolved}")
print(f"Missing from kindle-win32: {total - resolved}")
for dll, fn_list in missing.items():
    if fn_list:
        print(f"  {dll} ({len(fn_list)}): {fn_list}")
