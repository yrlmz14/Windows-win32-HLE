#!/bin/sh
# ==============================================================================
#  Genuine Windows 95 Launcher for Amazon Kindle Paperwhite 5 (Tiny386 Engine)
# ==============================================================================

EXTENSION=/mnt/us/extensions/win32
TMP_DIR=/var/tmp/win32

# 1. Prevent Screensaver and suspend native Kindle UI
if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null
    lipc-set-prop -i com.lab126.pillow disableAppMode 1 2>/dev/null
fi

# Pause awesome window manager and cvm so Kindle OS doesn't capture touch
kill -STOP $(pidof awesome cvm) 2>/dev/null

# 2. Setup run directory in /var/tmp (FAT32 /mnt/us has noexec mount flag!)
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
cp -pf "$EXTENSION/bin/tiny386" "$TMP_DIR/tiny386"
if [ -f "$EXTENSION/bin/fbink" ]; then
    cp -pf "$EXTENSION/bin/fbink" "$TMP_DIR/fbink"
elif [ -f /mnt/us/koreader/fbink ]; then
    cp -pf /mnt/us/koreader/fbink "$TMP_DIR/fbink"
fi
cp -pf "$EXTENSION/bin/bios.bin" "$TMP_DIR/bios.bin"
cp -pf "$EXTENSION/bin/vgabios.bin" "$TMP_DIR/vgabios.bin"
chmod -R 777 "$TMP_DIR" 2>/dev/null

# Prepare win95.ini inside /var/tmp/win32 pointing to FAT32 disk image
cat << 'EOF' > "$TMP_DIR/win95.ini"
[pc]
bios = bios.bin
vga_bios = vgabios.bin
mem_size = 32M
vga_mem_size = 4M
hda = /mnt/us/extensions/win32/win95.img
fill_cmos = 1
vga_force_8dm = 0

[display]
width = 640
height = 480

[cpu]
gen = 4
fpu = 1
EOF

# 3. Set device permissions
chmod 666 /dev/fb0 2>/dev/null
chmod 666 /dev/input/event* 2>/dev/null

# 4. Clear screen to clean white
if [ -x "$TMP_DIR/fbink" ]; then
    "$TMP_DIR/fbink" -q -c 2>/dev/null
elif command -v eips >/dev/null 2>&1; then
    eips -c 2>/dev/null
fi

# 5. Cleanup trap to resume Kindle UI on exit
cleanup() {
    if command -v lipc-set-prop >/dev/null 2>&1; then
        lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
        lipc-set-prop -i com.lab126.pillow disableAppMode 0 2>/dev/null
    fi
    kill -CONT $(pidof awesome cvm) 2>/dev/null
    if [ -x "$TMP_DIR/fbink" ]; then
        "$TMP_DIR/fbink" -q -c 2>/dev/null
    elif command -v eips >/dev/null 2>&1; then
        eips -c 2>/dev/null
    fi
    rm -rf "$TMP_DIR"
    exit 0
}

trap cleanup EXIT INT TERM HUP

# 6. Execute Tiny386 from executable RAM disk
cd "$TMP_DIR" || exit 1
export PATH="$TMP_DIR:$PATH"
"$TMP_DIR/tiny386" win95.ini > "$EXTENSION/win95.log" 2>&1
