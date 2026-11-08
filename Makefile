# Makefile for Win32 HLE on Amazon Kindle Paperwhite 5 (PW5 / Bellatrix)

CC_ARM   ?= arm-linux-gnueabihf-gcc
CC_HOST  ?= gcc

CFLAGS_COMMON = -O2 -Wall -Wextra -std=gnu99 -I./src -D_POSIX_C_SOURCE=200809L
CFLAGS_ARM    = $(CFLAGS_COMMON) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -static
CFLAGS_HOST   = $(CFLAGS_COMMON)

LDFLAGS_ARM   = -lm
LDFLAGS_HOST  = -lgdi32 -lm

SRCS = src/main.c \
       src/x86_core.c \
       src/pe_loader.c \
       src/platform/eink_render.c \
       src/platform/kindle_touch.c \
       src/platform/vkeyboard.c \
       src/win32/win32_dispatch.c \
       src/win32/kernel32.c \
       src/win32/user32.c \
       src/win32/gdi32.c \
       src/win32/advapi32.c \
       src/win32/comdlg32.c \
       src/win32/msvcrt.c \
       src/win32/cards.c

PACKAGE_DIR = package/extensions/win32
BIN_ARM     = $(PACKAGE_DIR)/bin/kindle_win32
BIN_WIN     = win32_sim.exe

.PHONY: all arm win win32hle test_win710 package clean

all: win win32hle

# Windows Desktop Simulator (builds with MinGW GCC on Windows)
win: $(BIN_WIN)
	@echo "[BUILD] Successfully compiled Windows Desktop Simulator: $(BIN_WIN)"

$(BIN_WIN): $(SRCS)
	$(CC_HOST) $(CFLAGS_HOST) $(SRCS) $(LDFLAGS_HOST) -o $(BIN_WIN)

# Windows 7 & 10 HLE CLI Runtime
win32hle:
	$(MAKE) -C win32hle

test_win710: win32hle
	@echo [TEST] Running Windows 7 Test Suite...
	-./win32hle/win32hle.exe win32hle/tests/test_win7.exe
	@echo [TEST] Running Windows 10 Test Suite...
	-./win32hle/win32hle.exe win32hle/tests/test_win10.exe
	@echo [TEST] Running ApiSet Redirection Suite...
	-./win32hle/win32hle.exe win32hle/tests/test_apiset.exe
	@echo [TEST] Running 512MB RAM Budget Enforcement Test...
	-./win32hle/win32hle.exe --ram-limit 4 win32hle/tests/test_memlimit.exe
	@echo [TEST] Running Extended Windows 7 and 10 Suite...
	-./win32hle/win32hle.exe win32hle/tests/test_extended.exe
	@echo [TEST] All Windows 7 and 10 HLE test suites PASSED!

# ARM Kindle Static Binary (builds with arm-linux-gnueabihf-gcc)
arm: $(BIN_ARM)
	@echo "[BUILD] Successfully compiled ARMv7 static binary for Kindle PW5: $(BIN_ARM)"

$(BIN_ARM): $(SRCS)
	@mkdir -p $(PACKAGE_DIR)/bin
	$(CC_ARM) $(CFLAGS_ARM) $(SRCS) $(LDFLAGS_ARM) -o $(BIN_ARM)

package: arm
	@mkdir -p dist
	tar -czf dist/win32-kual-extension.tar.gz -C package/extensions win32
	@echo "[PACKAGE] Generated dist/win32-kual-extension.tar.gz"

clean:
	$(MAKE) -C win32hle clean
ifeq ($(OS),Windows_NT)
	-del /Q /F $(BIN_WIN) *.exe *.o 2>NUL
else
	rm -f $(BIN_WIN) *.exe *.o
endif
