import struct

with open('C:/kindle-win32/apps/winmine_orig.exe', 'rb') as f:
    data = f.read()

rsrc_base = 0x4200

def parse_rsrc(off, path=()):
    flags, timedate, vmajor, vminor, num_named, num_id = struct.unpack('<IIHHHH', data[off:off+16])
    total = num_named + num_id
    for i in range(total):
        e_off = off + 16 + i*8
        id_val, off_val = struct.unpack('<II', data[e_off:e_off+8])
        is_dir = (off_val & 0x80000000) != 0
        child = rsrc_base + (off_val & 0x7FFFFFFF)
        if is_dir:
            parse_rsrc(child, path + (id_val,))
        else:
            data_rva, data_sz, cp, res = struct.unpack('<IIII', data[child:child+16])
            file_off = 0x4200 + (data_rva - 0x6000)
            if path and path[0] == 6: # STRING table
                block_id = path[1]
                boff = file_off
                for s_idx in range(16):
                    slen = struct.unpack('<H', data[boff:boff+2])[0]
                    boff += 2
                    if slen > 0:
                        s_str = data[boff:boff+slen*2].decode('utf-16le', errors='ignore')
                        boff += slen*2
                        sid = (block_id - 1)*16 + s_idx
                        print(f'STRING ID {sid}: "{s_str}"')

parse_rsrc(rsrc_base)
