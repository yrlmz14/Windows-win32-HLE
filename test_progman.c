#include "common.h"
#include "pe_loader.h"
#include "x86_core.h"
#include "win32/win32_dispatch.h"
#include "win32/user32.h"
#include <stdio.h>

void win32_app_request_load(const char *path) {
    printf("[TEST] WinExec requested app load: %s\n", path ? path : "(null)");
}

int main(void) {
    win32_process_t proc;
    memset(&proc, 0, sizeof(proc));
    guest_mem_init(&proc.mem);
    win32_init_subsystem(&proc);

    printf("=== Testing PROGMAN.EXE Execution in Win32 HLE ===\n");
    printf("Loading apps/progman.exe...\n");
    if (!pe_load_file(&proc, "apps/progman.exe")) {
        printf("FAILED to load PE!\n");
        return 1;
    }

    printf("PE Loaded successfully! EntryPoint=0x%08X, ESP=0x%08X\n", proc.cpu.regs.eip, proc.cpu.regs.esp);

    /* Step instructions to execute WinMain and create MDI Program Manager window */
    int steps = 0;
    int api_calls = 0;
    for (steps = 0; steps < 30000 && proc.cpu.running; steps++) {
        uint32_t cur_eip = proc.cpu.regs.eip;

        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            api_calls++;
            win32_dispatch_api(&proc, api_id);
            continue;
        }

        if (!x86_step(&proc.cpu)) {
            printf("[Step %d] x86_step stopped at EIP=0x%08X\n", steps, cur_eip);
            break;
        }
    }

    printf("Execution summary:\n");
    printf("  Steps executed: %d\n", steps);
    printf("  API calls dispatched: %d\n", api_calls);
    printf("  EIP=0x%08X, ESP=0x%08X, running=%d\n", proc.cpu.regs.eip, proc.cpu.regs.esp, proc.cpu.running);

    /* Check windows created */
    H_HWND act = user_get_active_window();
    printf("  Active HWND: 0x%08X\n", act);
    win32_window_t *w = user_get_window(act);
    if (w) {
        printf("  Window Title: \"%s\"\n", w->title);
        printf("  Window Class: \"%s\"\n", w->class_name);
        printf("  Window Geometry: %dx%d at (%d, %d)\n", w->width, w->height, w->x, w->y);
    }

    /* Save rendered desktop screenshot to BMP */
    int screen_w = 800;
    int screen_h = 600;
    int stride = screen_w;
    uint8_t *canvas = (uint8_t *)malloc(screen_w * screen_h);
    if (canvas) {
        memset(canvas, 0xE0, screen_w * screen_h); /* Light gray desktop */
        user_composite_windows(canvas, screen_w, screen_h, stride);

        FILE *bf = fopen("progman_desktop.bmp", "wb");
        if (bf) {
            uint32_t row_bytes = (screen_w * 3 + 3) & ~3;
            uint32_t img_size = row_bytes * screen_h;
            uint32_t file_size = 54 + img_size;
            uint8_t hdr[54] = {
                'B', 'M',
                file_size & 0xFF, (file_size >> 8) & 0xFF, (file_size >> 16) & 0xFF, (file_size >> 24) & 0xFF,
                0, 0, 0, 0,
                54, 0, 0, 0,
                40, 0, 0, 0,
                screen_w & 0xFF, (screen_w >> 8) & 0xFF, (screen_w >> 16) & 0xFF, (screen_w >> 24) & 0xFF,
                screen_h & 0xFF, (screen_h >> 8) & 0xFF, (screen_h >> 16) & 0xFF, (screen_h >> 24) & 0xFF,
                1, 0,
                24, 0,
                0, 0, 0, 0,
                img_size & 0xFF, (img_size >> 8) & 0xFF, (img_size >> 16) & 0xFF, (img_size >> 24) & 0xFF,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            fwrite(hdr, 1, 54, bf);
            uint8_t *row_buf = (uint8_t *)calloc(row_bytes, 1);
            for (int y = screen_h - 1; y >= 0; y--) {
                for (int x = 0; x < screen_w; x++) {
                    uint8_t val = canvas[y * stride + x];
                    row_buf[x * 3 + 0] = val;
                    row_buf[x * 3 + 1] = val;
                    row_buf[x * 3 + 2] = val;
                }
                fwrite(row_buf, 1, row_bytes, bf);
            }
            free(row_buf);
            fclose(bf);
            printf("[TEST] Successfully dumped progman_desktop.bmp (%dx%d)!\n", screen_w, screen_h);
        }
        free(canvas);
    }

    /* Inject double-click onto Minesweeper icon window (HWND 0x00004004) */
    printf("\n--- Injecting Double Click onto HWND 0x00004004 ---\n");
    user_post_message((H_HWND)0x00004004, 0x0203 /* WM_LBUTTONDBLCLK */, 1, 0x000A000A);

    for (int i = 0; i < 10000 && proc.cpu.running; i++) {
        uint32_t cur_eip = proc.cpu.regs.eip;
        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            api_calls++;
            win32_dispatch_api(&proc, api_id);
            continue;
        }
        if (!x86_step(&proc.cpu)) {
            printf("[Post-click Step %d] stopped at EIP=0x%08X\n", i, cur_eip);
            break;
        }
    }

    printf("=== Test PROGMAN.EXE completed successfully! ===\n");
    guest_mem_free(&proc.mem);
    return 0;
}
