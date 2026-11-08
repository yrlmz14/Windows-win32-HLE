import struct

def make_group_file(title, progs, out_filename):
    # progs: list of dict(name, cmdline, iconfile, x, y)
    num_progs = len(progs)
    
    # We will build data step-by-step
    # Header: 34 bytes
    # Program pointer table: num_progs * 2 bytes
    title_offset = 34 + num_progs * 2
    title_bytes = title.encode('ascii') + b'\0'
    
    curr_offset = title_offset + len(title_bytes)
    
    prog_ptrs = []
    prog_blocks = []
    
    for i, p in enumerate(progs):
        name_bytes = p['name'].encode('ascii') + b'\0'
        cmd_bytes = p['cmdline'].encode('ascii') + b'\0'
        ico_bytes = p['iconfile'].encode('ascii') + b'\0'
        
        # We need:
        # 24 bytes header
        # strings: name, cmd, ico
        # iconinfo: 12 bytes
        # AND mask: 128 bytes
        # XOR mask: 128 bytes
        
        prog_header_offset = curr_offset
        prog_ptrs.append(prog_header_offset)
        
        str_offset = prog_header_offset + 24
        name_ptr = str_offset
        cmd_ptr = name_ptr + len(name_bytes)
        ico_ptr = cmd_ptr + len(cmd_bytes)
        
        iconinfo_ptr = ico_ptr + len(ico_bytes)
        and_ptr = iconinfo_ptr + 12
        xor_ptr = and_ptr + 128
        
        # 24-byte program header:
        # x (2), y (2), nIconIndex (2), icontype=0x000c (2), iconANDsize (2), iconXORsize (2),
        # iconinfo_ptr (2), iconANDbits_ptr (2), iconXORbits_ptr (2),
        # name_ptr (2), cmd_ptr (2), iconfile_ptr (2)
        prog_hdr = struct.pack(
            '<hhHHHHHHHHHH',
            p['x'], p['y'],
            0, # nIconIndex
            0x000c, # icontype
            128, # iconANDsize
            128, # iconXORsize
            iconinfo_ptr,
            and_ptr,
            xor_ptr,
            name_ptr,
            cmd_ptr,
            ico_ptr
        )
        
        # Iconinfo (12 bytes):
        # xHotSpot(2), yHotSpot(2), width(2)=32, height(2)=32, planes(1)=1, bpp(1)=1
        iconinfo = struct.pack('<hhHHHH', 0, 0, 32, 32, 1, 1)
        
        and_mask = b'\x00' * 128 # transparent/opaque mask
        xor_mask = b'\x55' * 128 # checkerboard pattern
        
        block = prog_hdr + name_bytes + cmd_bytes + ico_bytes + iconinfo + and_mask + xor_mask
        prog_blocks.append(block)
        curr_offset += len(block)
        
    ext_offset = curr_offset
    
    # Build 34-byte header:
    # 0..3: 'PMXX'
    # 4..5: Checksum = 0
    # 6..7: Extension offset
    # 8..9: nCmdShow = SW_SHOWNORMAL (1)
    # 10..11: x = 15
    # 12..13: y = 15
    # 14..15: width = 440
    # 16..17: height = 280
    # 18..19: iconx = 0
    # 20..21: icony = 0
    # 22..23: Title offset
    # 24..31: unknown
    # 32..33: NumProg
    header = struct.pack(
        '<4sHHHhhHHhhHHHHHH',
        b'PMXX',
        0, # checksum
        ext_offset,
        1, # SW_SHOWNORMAL
        15, 15, # x, y
        440, 280, # width, height
        0, 0, # iconx, icony
        title_offset,
        0x0020, 0x0020, 0x0108, 0x0000,
        num_progs
    )
    
    ptrs_data = b''.join(struct.pack('<H', ptr) for ptr in prog_ptrs)
    
    final_data = header + ptrs_data + title_bytes + b''.join(prog_blocks)
    with open(out_filename, 'wb') as f:
        f.write(final_data)
    print(f"Generated {out_filename} ({len(final_data)} bytes) with {num_progs} programs")

if __name__ == '__main__':
    progs = [
        {'name': 'Minesweeper', 'cmdline': 'apps/winmine.exe', 'iconfile': 'apps/winmine.exe', 'x': 30, 'y': 30},
        {'name': 'Solitaire',   'cmdline': 'apps/sol.exe',     'iconfile': 'apps/sol.exe',     'x': 130, 'y': 30},
    ]
    make_group_file('Main', progs, 'C:/kindle-win32/apps/main.grp')
