#include "desktop_shell.h"
#include "win95_logo3d.h"
#include "platform/eink_render.h"
#include "platform/kindle_touch.h"
#include "platform/vkeyboard.h"
#include "win32/user32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void win32_app_request_load(const char *path);

static bool s_menu_open = true;
static bool s_start_open = false;
static float s_anim_angle = 0.55f;
static float s_anim_wave = 0.0f;

typedef struct {
    const char *label;
    const char *exe_path;
} launcher_app_t;

static const launcher_app_t s_apps[] = {
    {"1. Minesweeper (Fast)", "apps/winmine.exe"},
    {"2. Solitaire (Classic)", "apps/sol.exe"},
    {"3. Winmine (Win95)",     "apps/winmine.exe"},
    {"4. Notepad 95",          "apps/notepad.exe"},
    {"5. Paint Studio",        "apps/paint.exe"},
    {"6. Program Manager",     "apps/progman.exe"}
};
static const int s_app_count = sizeof(s_apps) / sizeof(s_apps[0]);

/* Built-in crisp 8x8 font table */
static const uint8_t FONT_8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* '#' */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* '$' */
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00}, /* '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* '(' */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* '*' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00}, /* ',' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* '.' */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* '/' */
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* '0' */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* '1' */
    {0x3C,0x66,0x06,0x1C,0x30,0x66,0x7E,0x00}, /* '2' */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* '3' */
    {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, /* '4' */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* '5' */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* '6' */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* '7' */
    {0x3C,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00}, /* '8' */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* '9' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* ':' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ';' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* '<' */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* '=' */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* '>' */
    {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00}, /* '?' */
    {0x3C,0x42,0x99,0xA1,0xA5,0x99,0x42,0x3C}, /* '@' */
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* 'A' */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* 'B' */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* 'C' */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* 'D' */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, /* 'E' */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, /* 'F' */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, /* 'G' */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'I' */
    {0x0E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, /* 'J' */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* 'K' */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* 'L' */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* 'M' */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, /* 'N' */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 'O' */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* 'P' */
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, /* 'Q' */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, /* 'R' */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* 'S' */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 'T' */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 'U' */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 'W' */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* 'X' */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* 'Y' */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* 'Z' */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* '[' */
    {0x60,0x30,0x18,0x0C,0x06,0x02,0x01,0x00}, /* '\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ']' */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00}, /* 'a' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* 'b' */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* 'c' */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* 'd' */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* 'e' */
    {0x1C,0x30,0x78,0x30,0x30,0x30,0x30,0x00}, /* 'f' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C}, /* 'g' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00}, /* 'j' */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* 'm' */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* 'o' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* 'p' */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, /* 'q' */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, /* 'r' */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* 's' */
    {0x30,0x30,0x78,0x30,0x30,0x34,0x18,0x00}, /* 't' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3B,0x00}, /* 'u' */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00}, /* 'w' */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* 'x' */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, /* 'y' */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* 'z' */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* '|' */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  /* '~' */
};

static void fill_rect(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                      int x, int y, int w, int h, uint8_t color) {
    for (int cy = y; cy < y + h; cy++) {
        if (cy < 0 || cy >= canvas_h) continue;
        uint8_t *row = canvas + (cy * stride);
        for (int cx = x; cx < x + w; cx++) {
            if (cx < 0 || cx >= canvas_w) continue;
            row[cx] = color;
        }
    }
}

static void draw_3d_rect(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                         int x, int y, int w, int h, uint8_t c_top_left, uint8_t c_bot_right) {
    for (int cx = x; cx < x + w; cx++) {
        if (cx >= 0 && cx < canvas_w) {
            if (y >= 0 && y < canvas_h) canvas[y * stride + cx] = c_top_left;
            if (y + h - 1 >= 0 && y + h - 1 < canvas_h) canvas[(y + h - 1) * stride + cx] = c_bot_right;
        }
    }
    for (int cy = y; cy < y + h; cy++) {
        if (cy >= 0 && cy < canvas_h) {
            if (x >= 0 && x < canvas_w) canvas[cy * stride + x] = c_top_left;
            if (x + w - 1 >= 0 && x + w - 1 < canvas_w) canvas[cy * stride + (x + w - 1)] = c_bot_right;
        }
    }
}

static void draw_string_scaled(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                               int x, int y, const char *str, uint8_t color, int scale) {
    if (!str || scale <= 0) return;
    int cur_x = x;
    while (*str) {
        char ch = *str++;
        if (ch < 32 || ch > 126) ch = ' ';
        const uint8_t *glyph = FONT_8X8[ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (row & (0x80 >> gx)) {
                    for (int sy = 0; sy < scale; sy++) {
                        int py = y + gy * scale + sy;
                        if (py < 0 || py >= canvas_h) continue;
                        uint8_t *crow = canvas + (py * stride);
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cur_x + gx * scale + sx;
                            if (px >= 0 && px < canvas_w) {
                                crow[px] = color;
                            }
                        }
                    }
                }
            }
        }
        cur_x += 8 * scale;
    }
}

void desktop_shell_init(void) {
    s_menu_open = true;
    s_start_open = false;
    s_anim_angle = 0.55f;
    s_anim_wave = 0.0f;
}

void desktop_shell_step_anim(void) {
    s_anim_angle += 0.04f;
    if (s_anim_angle > 6.2831853f) s_anim_angle -= 6.2831853f;
    s_anim_wave += 0.07f;
    if (s_anim_wave > 6.2831853f) s_anim_wave -= 6.2831853f;
}

bool desktop_shell_is_menu_open(void) {
    return s_menu_open;
}

void desktop_shell_set_menu_open(bool open) {
    s_menu_open = open;
}

void desktop_shell_toggle_menu(void) {
    s_menu_open = !s_menu_open;
}

bool desktop_shell_is_start_open(void) {
    return s_start_open;
}

void desktop_shell_toggle_start(void) {
    s_start_open = !s_start_open;
}

void desktop_shell_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride) {
    if (!canvas || canvas_w <= 0 || canvas_h <= 0) return;

    int taskbar_y = canvas_h - TASKBAR_HEIGHT;

    /* 1. Desktop Background */
    fill_rect(canvas, canvas_w, canvas_h, stride, 0, 0, canvas_w, taskbar_y, 0x80);

    /* 2. Main Retro Menu / Dialog (if open) */
    if (s_menu_open) {
        int win_w = 720;
        int win_h = 500;
        if (win_w > canvas_w - 20) win_w = canvas_w - 20;
        if (win_h > taskbar_y - 20) win_h = taskbar_y - 20;

        int win_x = (canvas_w - win_w) / 2;
        int win_y = (taskbar_y - win_h) / 2;

        /* Window Frame and Border */
        fill_rect(canvas, canvas_w, canvas_h, stride, win_x, win_y, win_w, win_h, 0xC0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, win_x, win_y, win_w, win_h, 0xFF, 0x00);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, win_x + 1, win_y + 1, win_w - 2, win_h - 2, 0xDF, 0x80);

        /* Titlebar */
        fill_rect(canvas, canvas_w, canvas_h, stride, win_x + 3, win_y + 3, win_w - 6, 24, 0x00);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 8, win_y + 7,
                           "Microsoft Windows 95 -- Retro Win32 Launcher", 0xFF, 1);

        /* Left Pane: 3D Rotating Logo Showcase */
        int left_w = win_w * 47 / 100;
        fill_rect(canvas, canvas_w, canvas_h, stride, win_x + 12, win_y + 34, left_w, win_h - 46, 0xD0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, win_x + 12, win_y + 34, left_w, win_h - 46, 0x80, 0xFF);

        /* Real-time 3D Rotating Windows 95 Logo */
        int logo_cx = win_x + 12 + left_w / 2;
        int logo_cy = win_y + 34 + 135;
        win95_logo3d_render(canvas, canvas_w, canvas_h, stride, logo_cx, logo_cy, s_anim_angle, s_anim_wave);

        /* Logo Captions */
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 50, win_y + 34 + 275, "Microsoft", 0x40, 2);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 40, win_y + 34 + 300, "Windows 95", 0x00, 3);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 35, win_y + 34 + 342, "Kindle PW5 Win32 HLE", 0x30, 2);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 28, win_y + 34 + 378, "E-Ink Carta 1200 | 1GB RAM", 0x50, 1);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 46, win_y + 34 + 398, "Octa-Core ARM Subsystem", 0x50, 1);

        /* Right Pane: App Menu Cards */
        int right_x = win_x + left_w + 22;
        int right_w = win_w - left_w - 34;

        draw_string_scaled(canvas, canvas_w, canvas_h, stride, right_x + 2, win_y + 38,
                           "APPLICATIONS & GAMES", 0x00, 2);

        for (int i = 0; i < s_app_count; i++) {
            int by = win_y + 72 + i * 54;
            fill_rect(canvas, canvas_w, canvas_h, stride, right_x, by, right_w, 44, 0xC0);
            draw_3d_rect(canvas, canvas_w, canvas_h, stride, right_x, by, right_w, 44, 0xFF, 0x00);
            draw_3d_rect(canvas, canvas_w, canvas_h, stride, right_x + 1, by + 1, right_w - 2, 42, 0xDF, 0x80);

            /* Number badge */
            fill_rect(canvas, canvas_w, canvas_h, stride, right_x + 8, by + 6, 32, 32, 0xFF);
            draw_3d_rect(canvas, canvas_w, canvas_h, stride, right_x + 8, by + 6, 32, 32, 0x80, 0xFF);
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            draw_string_scaled(canvas, canvas_w, canvas_h, stride, right_x + 19, by + 14, num, 0x00, 2);

            /* App title */
            draw_string_scaled(canvas, canvas_w, canvas_h, stride, right_x + 48, by + 14, s_apps[i].label, 0x00, 2);
        }

        /* Action Buttons: [ FLASH ] and [ EXIT ] */
        int btn_w = (right_w - 12) / 2;
        int by_act = win_y + 72 + s_app_count * 54;

        fill_rect(canvas, canvas_w, canvas_h, stride, right_x, by_act, btn_w, 40, 0xC0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, right_x, by_act, btn_w, 40, 0xFF, 0x00);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, right_x + 18, by_act + 13, "FLASH", 0x00, 2);

        fill_rect(canvas, canvas_w, canvas_h, stride, right_x + btn_w + 12, by_act, btn_w, 40, 0xC0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, right_x + btn_w + 12, by_act, btn_w, 40, 0xFF, 0x00);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, right_x + btn_w + 24, by_act + 13, "EXIT", 0x00, 2);
    }

    /* 3. Taskbar */
    fill_rect(canvas, canvas_w, canvas_h, stride, 0, taskbar_y, canvas_w, TASKBAR_HEIGHT, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, 0, taskbar_y, canvas_w, TASKBAR_HEIGHT, 0xFF, 0x80);

    /* Start Button */
    uint8_t start_col = s_menu_open ? 0xA0 : 0xC0;
    fill_rect(canvas, canvas_w, canvas_h, stride, 4, taskbar_y + 4, 90, 32, start_col);
    if (s_menu_open) {
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, 4, taskbar_y + 4, 90, 32, 0x00, 0xFF);
    } else {
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, 4, taskbar_y + 4, 90, 32, 0xFF, 0x00);
    }
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, 18, taskbar_y + 11, "Start", 0x00, 2);

    /* Active Window Title Pill on Taskbar */
    H_HWND act = user_get_active_window();
    win32_window_t *w = user_get_window(act);
    if (w && !s_menu_open) {
        int pw = 200;
        fill_rect(canvas, canvas_w, canvas_h, stride, 102, taskbar_y + 4, pw, 32, 0xE0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, 102, taskbar_y + 4, pw, 32, 0x00, 0xFF);
        char win_title[32];
        snprintf(win_title, sizeof(win_title), "%.20s", w->title[0] ? w->title : "Application");
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, 112, taskbar_y + 12, win_title, 0x00, 1);
    }

    /* System Tray Right Buttons: [KB] [R-CLICK] [FLASH] [EXIT] [TIME] */
    int rx = canvas_w - 6;

    /* Clock */
    rx -= 62;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm ? tm->tm_hour : 12, tm ? tm->tm_min : 0);
    fill_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 58, 30, 0xB0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 58, 30, 0x80, 0xFF);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, rx + 10, taskbar_y + 12, time_str, 0x00, 1);

    /* [EXIT] */
    rx -= 60;
    fill_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 54, 30, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 54, 30, 0xFF, 0x00);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, rx + 12, taskbar_y + 12, "EXIT", 0x00, 1);

    /* [FLASH] */
    rx -= 68;
    fill_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 62, 30, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 62, 30, 0xFF, 0x00);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, rx + 11, taskbar_y + 12, "FLASH", 0x00, 1);

    /* [R-CLICK] */
    rx -= 78;
    uint8_t rc_col = touch_is_right_click_mode() ? 0x00 : 0xC0;
    uint8_t rc_txt = touch_is_right_click_mode() ? 0xFF : 0x00;
    fill_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 72, 30, rc_col);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 72, 30, 0xFF, 0x00);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, rx + 10, taskbar_y + 12, "R-CLICK", rc_txt, 1);

    /* [KEYBD] */
    rx -= 70;
    uint8_t kb_col = vkeyboard_is_visible() ? 0x90 : 0xC0;
    fill_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 64, 30, kb_col);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, rx, taskbar_y + 5, 64, 30, 0xFF, 0x00);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, rx + 12, taskbar_y + 12, "KEYBD", 0x00, 1);
}

bool desktop_shell_handle_tap(int x, int y) {
    int canvas_w = DEFAULT_SCREEN_W;
    int canvas_h = DEFAULT_SCREEN_H;
    int stride = 0;
    eink_get_canvas(&canvas_w, &canvas_h, &stride);
    int taskbar_y = canvas_h - TASKBAR_HEIGHT;

    /* 1. Taskbar Taps */
    if (y >= taskbar_y) {
        /* Start Button */
        if (x >= 4 && x <= 94) {
            desktop_shell_toggle_menu();
            LOGI("Start button tapped -> Menu toggled (now %s)", s_menu_open ? "OPEN" : "CLOSED");
            return true;
        }

        /* Tray Buttons */
        int rx = canvas_w - 6;

        /* Clock */
        rx -= 62;

        /* EXIT */
        rx -= 60;
        if (x >= rx && x <= rx + 54) {
            LOGI("EXIT button tapped -> Terminating Win32 HLE");
            exit(0);
            return true;
        }

        /* FLASH */
        rx -= 68;
        if (x >= rx && x <= rx + 62) {
            LOGI("FLASH button tapped -> Full E-Ink refresh");
            eink_present(EINK_REFRESH_FULL);
            return true;
        }

        /* R-CLICK */
        rx -= 78;
        if (x >= rx && x <= rx + 72) {
            touch_set_right_click_mode(!touch_is_right_click_mode());
            LOGI("R-CLICK button tapped -> Right click mode is now %s",
                 touch_is_right_click_mode() ? "ON" : "OFF");
            return true;
        }

        /* KEYBD */
        rx -= 70;
        if (x >= rx && x <= rx + 64) {
            vkeyboard_toggle();
            LOGI("KEYBD button tapped -> Virtual keyboard is now %s",
                 vkeyboard_is_visible() ? "ON" : "OFF");
            return true;
        }

        return true; /* Consumed on taskbar */
    }

    /* 2. Menu Window Taps (when menu is open) */
    if (s_menu_open) {
        int win_w = 720;
        int win_h = 500;
        if (win_w > canvas_w - 20) win_w = canvas_w - 20;
        if (win_h > taskbar_y - 20) win_h = taskbar_y - 20;

        int win_x = (canvas_w - win_w) / 2;
        int win_y = (taskbar_y - win_h) / 2;

        int left_w = win_w * 47 / 100;
        int right_x = win_x + left_w + 22;
        int right_w = win_w - left_w - 34;

        /* Check app launcher cards */
        for (int i = 0; i < s_app_count; i++) {
            int by = win_y + 72 + i * 54;
            if (x >= right_x && x < right_x + right_w && y >= by && y < by + 44) {
                LOGI("Launcher selected app: %s (%s)", s_apps[i].label, s_apps[i].exe_path);
                s_menu_open = false;
                win32_app_request_load(s_apps[i].exe_path);
                return true;
            }
        }

        /* Action Buttons: FLASH and EXIT */
        int btn_w = (right_w - 12) / 2;
        int by_act = win_y + 72 + s_app_count * 54;

        /* FLASH */
        if (x >= right_x && x < right_x + btn_w && y >= by_act && y < by_act + 40) {
            LOGI("Launcher FLASH tapped");
            eink_present(EINK_REFRESH_FULL);
            return true;
        }

        /* EXIT */
        if (x >= right_x + btn_w + 12 && x < right_x + right_w && y >= by_act && y < by_act + 40) {
            LOGI("Launcher EXIT tapped");
            exit(0);
            return true;
        }

        return true; /* Consumed inside modal launcher menu */
    }

    return false;
}
