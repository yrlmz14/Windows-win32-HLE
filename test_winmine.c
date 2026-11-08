#include "common.h"
#include "pe_loader.h"
#include "x86_core.h"
#include "win32/win32_dispatch.h"
#include "win32/user32.h"
#include <stdio.h>

int main(void) {
    win32_process_t proc;
    memset(&proc, 0, sizeof(proc));
    guest_mem_init(&proc.mem);
    win32_init_subsystem(&proc);

    printf("Loading apps/winmine_orig.exe...\n");
    if (!pe_load_file(&proc, "apps/winmine_orig.exe")) {
        printf("FAILED to load PE!\n");
        return 1;
    }

    printf("PE Loaded successfully! EntryPoint=0x%08X, ESP=0x%08X\n", proc.cpu.regs.eip, proc.cpu.regs.esp);

    /* Step up to 20000 instructions to finish initialization and reach GetMessage */
    int steps = 0;
    for (steps = 0; steps < 20000 && proc.cpu.running; steps++) {
        uint32_t cur_eip = proc.cpu.regs.eip;
        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            win32_dispatch_api(&proc, api_id);
            continue;
        }

        if (!x86_step(&proc.cpu)) {
            printf("[Step %d] x86_step FAILED at EIP=0x%08X\n", steps, cur_eip);
            break;
        }
    }
    printf("Finished: executed %d steps. EIP=0x%08X, ESP=0x%08X, running=%d\n",
           steps, proc.cpu.regs.eip, proc.cpu.regs.esp, proc.cpu.running);

    /* Test sending WM_PAINT to winmine window */
    printf("\n--- Sending WM_PAINT to Winmine window (HWND 0x00004001) ---\n");
    user_post_message(0x00004001, WM_PAINT, 0, 0);

    /* Step more instructions to process WM_PAINT through message loop */
    for (int i = 0; i < 5000 && proc.cpu.running; i++) {
        uint32_t cur_eip = proc.cpu.regs.eip;
        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            win32_dispatch_api(&proc, api_id);
            continue;
        }

        if (!x86_step(&proc.cpu)) {
            printf("[Post-Paint Step %d] x86_step FAILED at EIP=0x%08X\n", i, cur_eip);
            break;
        }
    }

    printf("Post-Paint execution completed: EIP=0x%08X, ESP=0x%08X, running=%d\n",
           proc.cpu.regs.eip, proc.cpu.regs.esp, proc.cpu.running);

    /* Test clicking on cell (0, 0) */
    printf("\n--- Injecting WM_LBUTTONDOWN and WM_LBUTTONUP at cell (20, 65) ---\n");
    LPARAM click_lp = (uint32_t)(65 << 16) | (uint16_t)20;
    user_post_message(0x00004001, WM_LBUTTONDOWN, 1, click_lp);
    user_post_message(0x00004001, WM_LBUTTONUP, 0, click_lp);

    for (int i = 0; i < 10000 && proc.cpu.running; i++) {
        uint32_t cur_eip = proc.cpu.regs.eip;
        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            win32_dispatch_api(&proc, api_id);
            continue;
        }

        if (!x86_step(&proc.cpu)) {
            printf("[Post-Click Step %d] x86_step FAILED at EIP=0x%08X\n", i, cur_eip);
            break;
        }
    }

    /* Test right clicking on cell (36, 65) to place a flag */
    printf("\n--- Injecting WM_RBUTTONDOWN and WM_RBUTTONUP at cell (36, 65) ---\n");
    LPARAM rclick_lp = (uint32_t)(65 << 16) | (uint16_t)36;
    user_post_message(0x00004001, WM_RBUTTONDOWN, 2, rclick_lp);
    user_post_message(0x00004001, WM_RBUTTONUP, 0, rclick_lp);

    for (int i = 0; i < 10000 && proc.cpu.running; i++) {
        uint32_t cur_eip = proc.cpu.regs.eip;
        if ((cur_eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = cur_eip & ~HLE_API_MASK;
            win32_dispatch_api(&proc, api_id);
            continue;
        }

        if (!x86_step(&proc.cpu)) {
            printf("[Post-RClick Step %d] x86_step FAILED at EIP=0x%08X\n", i, cur_eip);
            break;
        }
    }

    printf("Post-RClick execution completed: EIP=0x%08X, ESP=0x%08X, running=%d\n",
           proc.cpu.regs.eip, proc.cpu.regs.esp, proc.cpu.running);

    win32_window_t *w = user_get_window(0x00004001);
    if (w && w->framebuffer) {
        int non_zero = 0;
        for (int i = 0; i < w->width * w->height * 4; i++) {
            if (w->framebuffer[i] != 0) non_zero++;
        }
        printf("Winmine Window (%dx%d) non-zero framebuffer bytes: %d / %d\n",
               w->width, w->height, non_zero, w->width * w->height * 4);

        FILE *f = fopen("winmine_surface.bmp", "wb");
        if (f) {
            uint32_t file_size = 54 + w->width * w->height * 4;
            uint8_t header[54] = {
                'B', 'M',
                file_size & 0xFF, (file_size >> 8) & 0xFF, (file_size >> 16) & 0xFF, (file_size >> 24) & 0xFF,
                0, 0, 0, 0,
                54, 0, 0, 0,
                40, 0, 0, 0,
                w->width & 0xFF, (w->width >> 8) & 0xFF, (w->width >> 16) & 0xFF, (w->width >> 24) & 0xFF,
                w->height & 0xFF, (w->height >> 8) & 0xFF, (w->height >> 16) & 0xFF, (w->height >> 24) & 0xFF,
                1, 0,
                32, 0,
                0, 0, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            fwrite(header, 1, 54, f);
            for (int y = w->height - 1; y >= 0; y--) {
                fwrite(w->framebuffer + y * w->width * 4, 1, w->width * 4, f);
            }
            fclose(f);
            printf("Saved winmine_surface.bmp successfully!\n");
        }
    }

    return 0;
}
