#include "desktop_shell.h"
#include "platform/eink_render.h"
#include "platform/kindle_touch.h"
#include "platform/vkeyboard.h"
#include "win32/user32.h"
#include <time.h>

static bool s_start_open = false;

typedef struct {
    const char *name;
    const char *exe_path;
} app_menu_item_t;

static const app_menu_item_t s_menu_items[] = {
    {"Minesweeper (Fast)", "apps/winmine.exe"},
    {"Solitaire (Classic Win32)", "apps/sol.exe"},
    {"Winmine 95 (Full)", "apps/winmine_orig.exe"},
    {"Notepad", "apps/notepad.exe"},
    {"Paint Studio", "apps/paint.exe"}
};
static const int s_menu_item_count = sizeof(s_menu_items) / sizeof(s_menu_items[0]);

void desktop_shell_init(void) {
    s_start_open = false;
}

void desktop_shell_toggle_start(void) {
    s_start_open = !s_start_open;
}

bool desktop_shell_is_start_open(void) {
    return s_start_open;
}

void desktop_shell_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride) {
    (void)canvas; (void)canvas_w; (void)canvas_h; (void)stride;

    /* 1. Desktop Background (Retro checkerboard dither) */
    eink_clear(EINK_LIGHT_GRAY);

    /* 1b. Emergency Exit Hotspot Button [X] at Top-Right Corner */
    eink_fill_rect(DEFAULT_SCREEN_W - 80, 8, 72, 36, EINK_BLACK);
    eink_draw_rect(DEFAULT_SCREEN_W - 80, 8, 72, 36, EINK_WHITE, 2);
    eink_draw_string(DEFAULT_SCREEN_W - 60, 20, "[X]", EINK_WHITE);

    /* 2. Taskbar (Bottom 48 pixels) */
    eink_fill_rect(0, TASKBAR_Y, DEFAULT_SCREEN_W, TASKBAR_HEIGHT, EINK_MID_GRAY);
    eink_fill_rect(0, TASKBAR_Y, DEFAULT_SCREEN_W, 2, EINK_WHITE);

    /* Start Button [ 90 x 36 ] */
    int btn_h = 36;
    int btn_y = TASKBAR_Y + 6;
    uint8_t start_col = s_start_open ? EINK_DARK_GRAY : EINK_NEAR_WHITE;
    eink_fill_rect(8, btn_y, 110, btn_h, start_col);
    eink_draw_rect(8, btn_y, 110, btn_h, EINK_BLACK, 2);
    eink_draw_string(24, btn_y + 12, "Start", EINK_BLACK);

    /* Active Window Title on Taskbar */
    H_HWND act = user_get_active_window();
    win32_window_t *w = user_get_window(act);
    if (w) {
        eink_fill_rect(126, btn_y, 220, btn_h, EINK_WHITE);
        eink_draw_rect(126, btn_y, 220, btn_h, EINK_BLACK, 2);
        char win_lbl[32];
        snprintf(win_lbl, sizeof(win_lbl), "%.24s", w->title[0] ? w->title : "Application");
        eink_draw_string(136, btn_y + 12, win_lbl, EINK_BLACK);
    }

    /* System Tray Right Buttons: [KB] [R-CLICK] [REFRESH] [EXIT] [TIME] */
    int rx = DEFAULT_SCREEN_W - 10;

    /* Clock */
    rx -= 70;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm ? tm->tm_hour : 12, tm ? tm->tm_min : 0);
    eink_fill_rect(rx, btn_y, 64, btn_h, EINK_DARK_GRAY);
    eink_draw_rect(rx, btn_y, 64, btn_h, EINK_BLACK, 1);
    eink_draw_string(rx + 12, btn_y + 12, time_str, EINK_WHITE);

    /* [EXIT] */
    rx -= 80;
    eink_fill_rect(rx, btn_y, 70, btn_h, EINK_WHITE);
    eink_draw_rect(rx, btn_y, 70, btn_h, EINK_BLACK, 2);
    eink_draw_string(rx + 16, btn_y + 12, "EXIT", EINK_BLACK);

    /* [REFRESH] */
    rx -= 100;
    eink_fill_rect(rx, btn_y, 90, btn_h, EINK_WHITE);
    eink_draw_rect(rx, btn_y, 90, btn_h, EINK_BLACK, 2);
    eink_draw_string(rx + 14, btn_y + 12, "FLASH", EINK_BLACK);

    /* [R-CLICK] Toggle */
    rx -= 110;
    uint8_t rc_col = touch_is_right_click_mode() ? EINK_BLACK : EINK_WHITE;
    uint8_t rc_txt = touch_is_right_click_mode() ? EINK_WHITE : EINK_BLACK;
    eink_fill_rect(rx, btn_y, 100, btn_h, rc_col);
    eink_draw_rect(rx, btn_y, 100, btn_h, EINK_BLACK, 2);
    eink_draw_string(rx + 14, btn_y + 12, "R-CLICK", rc_txt);

    /* [KEYBOARD] Toggle */
    rx -= 110;
    uint8_t kb_col = vkeyboard_is_visible() ? EINK_DARK_GRAY : EINK_WHITE;
    eink_fill_rect(rx, btn_y, 100, btn_h, kb_col);
    eink_draw_rect(rx, btn_y, 100, btn_h, EINK_BLACK, 2);
    eink_draw_string(rx + 16, btn_y + 12, "KEYBD", EINK_BLACK);

    /* 3. Start Menu (if open) */
    if (s_start_open) {
        int sm_w = 260;
        int sm_h = s_menu_item_count * 44 + 40;
        int sm_x = 8;
        int sm_y = TASKBAR_Y - sm_h;

        eink_fill_rect(sm_x, sm_y, sm_w, sm_h, EINK_NEAR_WHITE);
        eink_draw_rect(sm_x, sm_y, sm_w, sm_h, EINK_BLACK, 3);

        /* Start Menu Left Banner "Windows 95" */
        eink_fill_rect(sm_x + 2, sm_y + 2, 34, sm_h - 4, EINK_DARK_GRAY);
        eink_draw_string(sm_x + 8, sm_y + sm_h - 20, "W95", EINK_WHITE);

        /* Menu Items */
        for (int i = 0; i < s_menu_item_count; i++) {
            int iy = sm_y + 10 + (i * 44);
            eink_fill_rect(sm_x + 40, iy, sm_w - 46, 38, EINK_WHITE);
            eink_draw_rect(sm_x + 40, iy, sm_w - 46, 38, EINK_DARK_GRAY, 1);
            eink_draw_string(sm_x + 48, iy + 14, s_menu_items[i].name, EINK_BLACK);
        }
    }
}

bool desktop_shell_handle_tap(int x, int y) {
    /* 0. Top-Right Emergency Exit Button [X] */
    if (x >= DEFAULT_SCREEN_W - 90 && y <= 60) {
        LOGI("Top-Right Exit Button [X] tapped -> Exiting Win32 HLE");
        exit(0);
        return true;
    }

    /* 1. Start Menu Taps */
    if (s_start_open) {
        int sm_w = 260;
        int sm_h = s_menu_item_count * 44 + 40;
        int sm_x = 8;
        int sm_y = TASKBAR_Y - sm_h;

        if (x >= sm_x && x < sm_x + sm_w && y >= sm_y && y < sm_y + sm_h) {
            for (int i = 0; i < s_menu_item_count; i++) {
                int iy = sm_y + 10 + (i * 44);
                if (y >= iy && y < iy + 38) {
                    LOGI("Start Menu launch: %s (%s)", s_menu_items[i].name, s_menu_items[i].exe_path);
                    s_start_open = false;
                    /* Launch app */
                    extern void win32_app_request_load(const char *path);
                    win32_app_request_load(s_menu_items[i].exe_path);
                    return true;
                }
            }
            return true;
        } else {
            s_start_open = false;
            return true;
        }
    }

    /* 2. Taskbar Taps */
    if (y >= TASKBAR_Y) {
        /* Start Button */
        if (x >= 8 && x <= 118) {
            desktop_shell_toggle_start();
            return true;
        }

        /* Tray buttons */
        int rx = DEFAULT_SCREEN_W - 10;
        rx -= 70; /* Clock */

        /* [EXIT] */
        rx -= 80;
        if (x >= rx && x <= rx + 70) {
            LOGI("User clicked EXIT button -> Exiting Win32 HLE");
            exit(0);
            return true;
        }

        /* [FLASH] */
        rx -= 100;
        if (x >= rx && x <= rx + 90) {
            LOGI("User triggered E-Ink Full Flash");
            eink_present(EINK_REFRESH_FULL);
            return true;
        }

        /* [R-CLICK] */
        rx -= 110;
        if (x >= rx && x <= rx + 100) {
            touch_set_right_click_mode(!touch_is_right_click_mode());
            LOGI("Toggled Right-Click Mode: %s", touch_is_right_click_mode() ? "ON" : "OFF");
            return true;
        }

        /* [KEYBD] */
        rx -= 110;
        if (x >= rx && x <= rx + 100) {
            vkeyboard_toggle();
            LOGI("Toggled Virtual Keyboard: %s", vkeyboard_is_visible() ? "ON" : "OFF");
            return true;
        }

        return true; /* Consumed on taskbar */
    }

    return false;
}
