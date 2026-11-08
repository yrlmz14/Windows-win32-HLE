ARCH ?= native
CFLAGS ?= -O0 -g -Wall -Wextra -Iinclude -MMD -MP
LDFLAGS ?= -lpthread

ifeq ($(OS),Windows_NT)
  TARGET ?= win32hle.exe
  RM = del /Q /F
else
  RM = rm -f
  ifeq ($(ARCH),arm64)
    CC = clang -target aarch64-linux-gnu -fuse-ld=lld
    TARGET ?= win32hle-arm64
  else
    CC ?= gcc
    TARGET ?= win32hle
  endif
endif

SRC = src/main.c \
      src/memory.c \
      src/teb_peb.c \
      src/apiset.c \
      src/hle_dispatch.c \
      src/kernel32_base.c \
      src/kernel32_win7_10.c \
      src/ucrt_thunks.c \
      src/msvcrt_thunks.c \
      src/sync.c \
      src/pe_loader.c \
      src/cpu.c \
      src/user32_thunks.c \
      src/arm64_dynarec.c

OBJ = $(SRC:.c=.o)
DEP = $(SRC:.c=.d)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEP)

clean:
ifeq ($(OS),Windows_NT)
	-del /Q /F src\*.o src\*.d $(TARGET) win32hle.exe 2>NUL
else
	rm -f src/*.o src/*.d $(TARGET) win32hle win32hle-arm64
endif

.PHONY: all clean
