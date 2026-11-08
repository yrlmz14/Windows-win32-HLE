#include "common.h"
#include "pe_loader.h"
#include "x86_core.h"
#include "win32/win32_dispatch.h"
#include "win32/user32.h"
#include "platform/eink_render.h"
#include "platform/kindle_touch.h"
#include "platform/vkeyboard.h"
#include "desktop_shell.h"
#include <unistd.h>
#include <signal.h>

static win32_process_t s_proc;
static bool s_has_loaded_app = false;
static char s_pending_app[256] = "";

void win32_app_request_load(const char *path) {
    if (path) {
        strncpy(s_pending_app, path, sizeof(s_pending_app) - 1);
    }
}

static void sig_handler(int sig) {
    (void)sig;
    LOGI("Received exit signal, shutting down Win32 HLE");
    eink_cleanup();
    touch_cleanup();
    exit(0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    LOGI("=================================================================");
    LOGI("Kindle Paperwhite 5 Win32 HLE Engine & Retro Desktop");
    LOGI("Hardware: 1648x1236 E-Ink Carta 1200 | Octa-Core ARM | 1 GB RAM");
    LOGI("=================================================================");

    /* 1. Initialize Display & Input Subsystems */
    if (eink_init(DEFAULT_SCREEN_W, DEFAULT_SCREEN_H) != 0) {
        LOGE("Failed to initialize E-Ink display system");
        return 1;
    }
    touch_init();
    vkeyboard_init();
    desktop_shell_init();

    /* 2. Initialize Win32 Subsystem */
    guest_mem_init(&s_proc.mem);
    win32_init_subsystem(&s_proc);

    /* 3. Determine Initial App or Open Retro 3D Menu */
    if (argc > 1) {
        win32_app_request_load(argv[1]);
        desktop_shell_set_menu_open(false);
    } else {
        desktop_shell_set_menu_open(true);
    }

    LOGI("Starting Main Win32 Event Loop...");

    uint64_t frame_count = 0;
    bool screen_dirty = true;
    while (1) {
        frame_count++;

        /* Check for pending executable launch */
        if (s_pending_app[0] != '\0') {
            LOGI("Loading Win32 Application: %s", s_pending_app);
            char target[256];
            strncpy(target, s_pending_app, sizeof(target) - 1);
            s_pending_app[0] = '\0';

            /* Load executable */
            if (pe_load_file(&s_proc, target)) {
                s_has_loaded_app = true;
                desktop_shell_set_menu_open(false);
                screen_dirty = true;
                LOGI("Application loaded successfully!");
            } else {
                LOGE("Failed to load application: %s", target);
                desktop_shell_set_menu_open(true);
            }
        }

        /* Poll touch inputs */
        touch_poll();
        if (touch_take_dirty()) {
            screen_dirty = true;
        }

        /* Step 3D logo rotation animation when menu is open */
        if (desktop_shell_is_menu_open()) {
            desktop_shell_step_anim();
            screen_dirty = true;
        }

        /* Execute batch of x86 instructions for loaded Win32 process */
        if (s_has_loaded_app && s_proc.cpu.running) {
            for (int i = 0; i < 50000 && s_proc.cpu.running; i++) {
                if ((s_proc.cpu.regs.eip & HLE_API_MASK) == HLE_API_BASE) {
                    uint32_t api_id = s_proc.cpu.regs.eip & ~HLE_API_MASK;
                    win32_dispatch_api(&s_proc, api_id);
                    screen_dirty = true;
                    if (s_proc.cpu.yielded) {
                        s_proc.cpu.yielded = false;
                        break;
                    }
                    continue;
                }
                x86_step(&s_proc.cpu);
            }

            /* If app completed/terminated, bring back the 3D retro menu */
            if (!s_proc.cpu.running) {
                LOGI("Application exited cleanly. Returning to Retro Desktop Menu.");
                s_has_loaded_app = false;
                desktop_shell_set_menu_open(true);
                screen_dirty = true;
            }
        }

        /* Composite Frame */
        if (screen_dirty || frame_count <= 3) {
            screen_dirty = false;

            int canvas_w, canvas_h, canvas_stride;
            uint8_t *canvas = eink_get_canvas(&canvas_w, &canvas_h, &canvas_stride);

            /* A. Desktop Shell (Background, 3D Rotating Logo Menu, Taskbar) */
            desktop_shell_draw(canvas, canvas_w, canvas_h, canvas_stride);

            /* B. Active Windows (if not in modal launcher menu) */
            if (!desktop_shell_is_menu_open() && s_has_loaded_app) {
                user_composite_windows(canvas, canvas_w, canvas_h, canvas_stride);
            }

            /* C. On-Screen Virtual Keyboard (if visible) */
            if (vkeyboard_is_visible()) {
                vkeyboard_draw(canvas, canvas_w, canvas_h, canvas_stride);
            }

            /* Present to E-Ink Display / Simulator */
            int refresh_mode = (frame_count == 1 || frame_count % 300 == 0) ? EINK_REFRESH_FULL : EINK_REFRESH_FAST;
            eink_present(refresh_mode);
        }

        /* Yield CPU briefly for ~35-40 FPS UI loop */
        sys_sleep_ms(25);
    }

    /* Cleanup */
    guest_mem_free(&s_proc.mem);
    eink_cleanup();
    touch_cleanup();
    return 0;
}
