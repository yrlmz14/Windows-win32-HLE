# Windows 95 & Win32 HLE for Amazon Kindle Paperwhite 5

Boot genuine **Microsoft Windows 95** and run classic 16/32-bit Windows applications directly on the **Amazon Kindle Paperwhite 5** (11th Gen, Carta 1200 E-Ink display, Bellatrix platform) without X11, SDL, or VNC overhead.

![Platform](https://img.shields.io/badge/Platform-Amazon%20Kindle%20PW5%20(ARMv7)-orange)
![Architecture](https://img.shields.io/badge/Architecture-ARMhf%20%2F%20x86%20HLE-blue)
![Display](https://img.shields.io/badge/Display-Direct%20%2Fdev%2Ffb0%20E--Ink-black)
![Input](https://img.shields.io/badge/Input-Multi--Touch%20Type%20B-green)
![OS](https://img.shields.io/badge/OS-Windows%2095%20%2F%20Win32-purple)

---

## Overview

This repository contains two complementary execution engines tailored specifically for Kindle e-ink hardware:

1. **Bare-Metal PC Emulator (`emulator/tiny386`)**:
   - An optimized 386/486 PC emulator running SeaBIOS and genuine Windows 95 (`win95.img`).
   - Direct Linux framebuffer output (`/dev/fb0`) with 640×480 16-color VGA graphics.
   - Custom low-latency multi-touch digitizer driver (`rawdraw/CNFGKindleDriver.c`) reading `/dev/input/event1` (Linux Multi-Touch Protocol Type B).
   - Real-time PS/2 relative mouse delta calculation with gear ratio compensation.

2. **Pure-C Win32 High-Level Emulation (HLE) Runtime (`src/`)**:
   - A lightweight, native Win32 API layer (USER32, GDI32, KERNEL32, ADVAPI32, COMDLG32, MSVCRT).
   - Built-in PE executable loader and x86 user-mode CPU emulator (`x86_core.c`).
   - Direct 16-level grayscale e-ink rendering engine (`src/platform/eink_render.c`).
   - Runs original Windows 3.1/95/NT desktop applications natively:
     - Program Manager (`progman.exe`)
     - Minesweeper (`winmine.exe`)
     - Solitaire (`sol.exe`)
     - Notepad (`notepad.exe`)
     - Paint (`paint.exe`)

---

## Directory Structure

```text
├── apps/                        # Classic Windows applications (WinMine, Solitaire, Progman, etc.)
├── emulator/                    # PC Emulator subsystem
│   ├── bin/                     # Precompiled helper utilities (bios.bin, vgabios.bin)
│   ├── tiny386/                 # Forked tiny386 PC emulator source with Kindle rawdraw driver
│   │   └── rawdraw/             # Direct /dev/fb0 & /dev/input/event1 Kindle driver
│   └── win95.ini                # Emulator hardware configuration
├── package/                     # Ready-to-copy KUAL extension
│   └── extensions/
│       └── win32/
│           ├── bin/             # Precompiled ARMv7 binaries (tiny386, kindle_win32, fbink)
│           ├── config.xml       # KUAL extension manifest
│           ├── menu.json        # KUAL touch launcher menu
│           ├── run.sh           # Win32 HLE launcher script
│           ├── run_win95.sh     # Windows 95 emulator launcher script
│           └── win95.ini        # Target runtime configuration
├── src/                         # Win32 HLE implementation source code
│   ├── main.c                   # CLI driver and runtime entry point
│   ├── pe_loader.c / .h         # Portable Executable (PE32) loader
│   ├── x86_core.c / .h          # 32-bit x86 CPU emulator core
│   ├── platform/                # Kindle hardware drivers (fb0, touch, virtual keyboard)
│   └── win32/                   # Win32 subsystem DLL implementations
├── patch_win95_img.py           # Helper script to tune WIN.INI mouse sensitivity in win95.img
└── Makefile                     # Build system for host simulation and Kindle ARM binaries
```

---

## Hardware Requirements

- **Device**: Amazon Kindle Paperwhite 5 (11th Gen, 2021)
- **SoC**: NXP i.MX7Dual (Cortex-A7 @ 1.0 GHz, ARMv7-A)
- **Display**: 6.8" E-Ink Carta 1200 (1236 × 1648, 300 PPI)
- **Touch**: Linux Multi-Touch Protocol Type B digitizer on `/dev/input/event1`
- **Software**: Jailbroken Kindle OS (5.14.x - 5.16.x) with **KUAL** (Kindle Unified Application Launcher) installed.

---

## Touch Gestures & Hotkeys

Because standard PC operating systems expect keyboard and mouse input, the Kindle driver translates touchscreen interactions into standard PC inputs:

| Action | Physical Gesture |
| :--- | :--- |
| **Left Click** | Single tap anywhere on the screen |
| **Cursor Drag** | Touch and slide finger across screen |
| **Right Click** | Stationary touch & hold for **600ms** (without dragging) |
| **Send ENTER Key** | Tap the **Top-Left Corner** (dismisses boot/safe mode dialogs) |
| **Emergency Exit** | **Triple-tap Top-Right Corner** within 1.5s (returns cleanly to Kindle OS) |

---

## Installation on Kindle

### 1. Copy KUAL Extension
1. Connect your jailbroken Kindle to your PC via USB.
2. Copy the folder `package/extensions/win32` directly into your Kindle's `extensions/` directory:
   ```text
   Kindle Drive (e.g., E:\ or /media/kindle)
   └── extensions/
       └── win32/
           ├── bin/
           ├── config.xml
           ├── menu.json
           ├── run.sh
           ├── run_win95.sh
           └── win95.ini
   ```

### 2. Add Windows 95 Disk Image
Due to GitHub's file size limit (100 MB), the pre-installed Windows 95 disk image (`win95.img`, ~125.5 MB) is not tracked in git.

Place your bootable FAT16 Windows 95 raw disk image at:
```text
extensions/win32/win95.img
```

*(Optional)* Run `python patch_win95_img.py` to automatically configure `WINDOWS/WIN.INI` with 1:1 mouse scaling (`MouseSpeed=0`, `MouseThreshold1=0`, `MouseThreshold2=0`).

### 3. Launch from KUAL
1. Safely eject and unplug the Kindle from your computer.
2. Open **KUAL** from your Kindle library.
3. Tap **Windows 95 / Win32 HLE**.
4. Select **Start Windows 95** (or any of the native Win32 standalone apps).

---

## Building from Source

### Prerequisites
- Linux / WSL2 / MSYS2 environment
- ARMv7 cross-compiler toolchain: `arm-linux-gnueabihf-gcc`
- Host C compiler (GCC or Clang) for test simulators

### 1. Build Win32 HLE Runtime
```bash
# Build desktop host simulator
make

# Build Kindle ARMv7 native binary
make kindle
```

### 2. Build Tiny386 with Kindle Rawdraw Driver
```bash
cd emulator/tiny386
make -f Makefile.rawdraw \
    CC=arm-linux-gnueabihf-gcc \
    USE_FAST=y \
    EXTRA_CFLAGS="-DI386_ALIGNED_ONLY -mno-unaligned-access -static -O3"
```

The resulting `tiny386` static binary will be placed in `package/extensions/win32/bin/tiny386`.

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
All respective Windows trademarks and software components are property of Microsoft Corporation.
