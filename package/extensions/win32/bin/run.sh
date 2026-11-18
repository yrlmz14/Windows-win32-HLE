#!/bin/sh
# Name: Windows
# Author: Win32 HLE
# Icon: data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAM8AAAFLCAAAAABUvIf5AAABuklEQVR42u3bMY7CMBAFUDNyteJOS82BOB97Jzoie6vtKJzZkZDI+3W+yHOI03hOs31UovHw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDk03dcO0ZyzaKmv5LTPOrzGfFzj8QKx7h8j/h/fzFzNc95S67ZbT4L+mvZ8/6ce98S/4DtXNQv3w+2lrifto2ivu8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw/PO7LnvGX0nvqFKOpXex5b6vhnexT1az3Rrl/J89MtCvprOe75ePML9mse78+nPh/zP38x//O6X74fmP/x/eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHhOaDH/M/emP953V+L+Z/ECptfsF/z8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PDw8PC8Jb9p6ijKfornrAAAAABJRU5ErkJggg==
# DontUseFBInk
# Version: 1.0.0
# ==============================================================================
#  Win32 HLE Launcher for Amazon Kindle Paperwhite 5 (PW5 / Bellatrix)
# ==============================================================================

# Framework shutdown can also send SIGTERM/SIGHUP to this launcher.
trap ':' TERM HUP

# --- 0. Locate the extension folder (must contain bin/kindle_win32) -----------
SELF_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
EXTENSION=
for d in "$SELF_DIR/.." "$SELF_DIR" /mnt/us/extensions/win32 /mnt/us/win32; do
    if [ -n "$d" ] && [ -f "$d/bin/kindle_win32" ]; then
        EXTENSION=$(cd "$d" && pwd)
        break
    fi
done

# Log goes to /mnt/us (always writable) when the extension is not found
LOGDIR=${EXTENSION:-/mnt/us}
{ echo "date: $(date)"; echo "arg0: $0"; echo "self: $SELF_DIR"; echo "ext: $EXTENSION"; uname -a; ls -l /dev/fb0 /dev/input 2>&1; head -12 /proc/cpuinfo 2>&1; } > "$LOGDIR/diag.log" 2>&1

if [ -z "$EXTENSION" ]; then
    echo "ERROR: bin/kindle_win32 not found (searched $SELF_DIR/.., /mnt/us/extensions/win32)" >> "$LOGDIR/diag.log"
    exit 1
fi

TMP_DIR=/var/tmp/win32

# --- 1. Setup run directory in /var/tmp ---------------------------------------
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR" || exit 1
cp -pf "$EXTENSION/bin/kindle_win32" "$TMP_DIR/kindle_win32" || exit 1
chmod 755 "$TMP_DIR/kindle_win32"
if [ -f "$EXTENSION/bin/fbink" ]; then
    cp -pf "$EXTENSION/bin/fbink" "$TMP_DIR/fbink"
elif [ -f /mnt/us/koreader/fbink ]; then
    cp -pf /mnt/us/koreader/fbink "$TMP_DIR/fbink"
fi
mkdir -p "$TMP_DIR/apps"
[ -d "$EXTENSION/apps" ] && cp -prf "$EXTENSION/apps/." "$TMP_DIR/apps/" 2>/dev/null
chmod -R 777 "$TMP_DIR" 2>/dev/null

# --- 2. Device permissions ----------------------------------------------------
chmod 666 /dev/fb0 2>/dev/null
chmod 666 /dev/input/event* 2>/dev/null

# --- 3. Cleanup (restores the Kindle UI) --------------------------------------
cleanup() {
    trap - EXIT INT TERM HUP
    if command -v lipc-set-prop >/dev/null 2>&1; then
        lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
    fi
    if [ "$FRAMEWORK_STOPPED" = 1 ]; then
        start lab126_gui >/dev/null 2>&1
    else
        kill -CONT $(pidof awesome cvm) 2>/dev/null
    fi
    if [ -x "$TMP_DIR/fbink" ]; then
        "$TMP_DIR/fbink" -q -c 2>/dev/null
    elif command -v eips >/dev/null 2>&1; then
        eips -c 2>/dev/null
    fi
    rm -rf "$TMP_DIR"
    exit 0
}

# --- 4. Suspend native Kindle UI ----------------------------------------------
if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null
fi

FRAMEWORK_STOPPED=0
if [ -d /etc/upstart ] && command -v stop >/dev/null 2>&1; then
    # Modern firmware (PW5): same method KOReader uses
    stop lab126_gui >/dev/null 2>&1 && FRAMEWORK_STOPPED=1
    sleep 1
else
    # Legacy firmware
    kill -STOP $(pidof awesome cvm) 2>/dev/null
fi

trap cleanup EXIT INT TERM HUP

# --- 5. Clear screen ----------------------------------------------------------
if [ -x "$TMP_DIR/fbink" ]; then
    "$TMP_DIR/fbink" -q -c 2>/dev/null
elif command -v eips >/dev/null 2>&1; then
    eips -c 2>/dev/null
fi

# --- 6. Run the Win32 HLE engine (log flushed directly to USB storage) --------
cd "$TMP_DIR" || exit 1
export PATH="$TMP_DIR:$PATH"
"$TMP_DIR/kindle_win32" "$@" > "$EXTENSION/win32.log" 2>&1
