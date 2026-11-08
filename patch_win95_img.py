import os
import sys

def patch_win95_image(img_path):
    print(f"Opening {img_path}...")
    with open(img_path, "r+b") as f:
        def cluster_offset(c):
            return 306176 + (c - 2) * 2048

        # 1. Verify directory entry at 8830656
        f.seek(8830656)
        ent = f.read(32)
        if ent[:11] != b'SYSTEM  INI':
            raise ValueError(f"Expected SYSTEM.INI at 8830656, got {ent[:11]}")
        
        sys_cluster = int.from_bytes(ent[26:28], 'little')
        sys_size = int.from_bytes(ent[28:32], 'little')
        print(f"Found SYSTEM.INI: cluster={sys_cluster}, size={sys_size}")
        
        # 2. Read content from cluster
        f.seek(cluster_offset(sys_cluster))
        raw = f.read(sys_size)
        text = raw.decode('latin1')
        
        # 3. Apply patches
        orig_text = text
        text = text.replace("display.drv=pnpdrvr.drv", "display.drv=vga.drv")
        text = text.replace("display.drv=S3", "display.drv=VGA")
        text = text.replace("*DisplayFallback=0\r\n", "")
        text = text.replace("*DisplayFallback=0\n", "")
        
        if text == orig_text:
            print("No changes needed (already patched).")
            return
            
        new_bytes = text.encode('latin1')
        new_size = len(new_bytes)
        print(f"Patched SYSTEM.INI: original size {sys_size} -> new size {new_size}")
        
        if new_size > 2048:
            raise ValueError(f"New size {new_size} exceeds single cluster (2048)!")
            
        # 4. Write back content to cluster
        f.seek(cluster_offset(sys_cluster))
        f.write(new_bytes)
        # Zero out the rest of the cluster
        if 2048 - new_size > 0:
            f.write(b'\x00' * (2048 - new_size))
            
        # 5. Update directory entry file size
        f.seek(8830656 + 28)
        f.write(new_size.to_bytes(4, 'little'))
        print("Updated directory entry size successfully.")
        
    print("Done patching win95.img!")

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else r"C:\kindle-win32\package\extensions\win32\win95.img"
    patch_win95_image(target)
