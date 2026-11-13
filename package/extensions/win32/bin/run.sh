#!/bin/sh
# ==============================================================================
#  Win32 HLE Launcher for Amazon Kindle Paperwhite 5 (PW5 / Bellatrix)
# ==============================================================================

EXTENSION=$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)
[ -z "$EXTENSION" ] && EXTENSION=/mnt/us/extensions/win32
# Early diagnostics (works on Colorsoft / other layouts); written next to the script
{ echo "date: $(date)"; echo "ext: $EXTENSION"; uname -a; ls -l /dev/fb0 /dev/input 2>&1; cat /proc/cpuinfo 2>&1 | head -12; } > "$EXTENSION/diag.log" 2>&1
TMP_DIR=/var/tmp/win32

# 1. Prevent Screensaver and suspend native Kindle UI
if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null
    lipc-set-prop -i com.lab126.pillow disableAppMode 1 2>/dev/null
fi

# Pause awesome window manager and cvm so Kindle OS doesn't capture touch
kill -STOP $(pidof awesome cvm) 2>/dev/null

# 2. Setup run directory in /var/tmp
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
cp -pf "$EXTENSION/bin/kindle_win32" "$TMP_DIR/kindle_win32"
if [ -f "$EXTENSION/bin/fbink" ]; then
    cp -pf "$EXTENSION/bin/fbink" "$TMP_DIR/fbink"
elif [ -f /mnt/us/koreader/fbink ]; then
    cp -pf /mnt/us/koreader/fbink "$TMP_DIR/fbink"
fi

# Copy bundled apps
mkdir -p "$TMP_DIR/apps"
cp -prf "$EXTENSION/apps/"* "$TMP_DIR/apps/" 2>/dev/null
chmod -R 777 "$TMP_DIR" 2>/dev/null

# 3. Set device permissions
chmod 666 /dev/fb0 2>/dev/null
chmod 666 /dev/input/event* 2>/dev/null

# 4. Clear screen
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

# 6. Execute Win32 HLE Engine (direct flush to USB storage for instant diagnostic logging)
cd "$TMP_DIR" || exit 1
export PATH="$TMP_DIR:$PATH"
"$TMP_DIR/kindle_win32" "$@" > "$EXTENSION/win32.log" 2>&1
