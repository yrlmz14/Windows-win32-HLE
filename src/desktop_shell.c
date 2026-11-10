#include "desktop_shell.h"
#include "win95_logo3d.h"
#include "platform/eink_render.h"
#include "platform/kindle_touch.h"
#include "platform/vkeyboard.h"
#include "win32/user32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void win32_app_request_load(const char *path);

static bool s_menu_open = true;
static float s_anim_angle = 0.55f;
static float s_anim_wave = 0.0f;

/* Built-in crisp 8x8 font table */
static const uint8_t FONT_8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* '#' */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* '$' */
    {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00}, /* '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* '\'' */
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
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* '\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ']' */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, /* 'a' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* 'b' */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* 'c' */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* 'd' */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* 'e' */
    {0x0E,0x18,0x7E,0x18,0x18,0x18,0x18,0x00}, /* 'f' */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, /* 'g' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
    {0x06,0x00,0x06,0x06,0x06,0x06,0x66,0x3C}, /* 'j' */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* 'm' */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* 'o' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* 'p' */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07}, /* 'q' */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, /* 'r' */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* 's' */
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, /* 't' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* 'u' */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 'w' */
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
            if (cx >= 0 && cx < canvas_w) {
                row[cx] = color;
            }
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
        if (ch >= 32 && ch <= 126) {
            const uint8_t *glyph = FONT_8X8[ch - 32];
            for (int r = 0; r < 8; r++) {
                uint8_t row_bits = glyph[r];
                for (int c = 0; c < 8; c++) {
                    if (row_bits & (0x80 >> c)) {
                        for (int sy = 0; sy < scale; sy++) {
                            int py = y + r * scale + sy;
                            if (py < 0 || py >= canvas_h) continue;
                            uint8_t *p_row = canvas + (py * stride);
                            for (int sx = 0; sx < scale; sx++) {
                                int px = cur_x + c * scale + sx;
                                if (px >= 0 && px < canvas_w) {
                                    p_row[px] = color;
                                }
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
    s_anim_angle = 0.55f;
    s_anim_wave = 0.0f;
}

void desktop_shell_step_anim(void) {
    s_anim_angle += 0.055f;
    s_anim_wave += 0.14f;
    if (s_anim_angle > 6.28318f) s_anim_angle -= 6.28318f;
    if (s_anim_wave > 6.28318f) s_anim_wave -= 6.28318f;
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
    return false;
}

void desktop_shell_toggle_start(void) {
    desktop_shell_toggle_menu();
}

static void launch_windows_95(void) {
    LOGI("Launching Genuine Windows 95...");
#ifdef _WIN32
    if (access("emulator/bin/win32/tiny386.exe", F_OK) == 0) {
        system("start emulator/bin/win32/tiny386.exe emulator/win95.ini");
    } else {
        LOGI("Tiny386 emulator binary not found in emulator/bin/win32/");
    }
#else
    if (access("/mnt/us/extensions/win32/bin/run_win95.sh", X_OK) == 0) {
        system("/bin/sh /mnt/us/extensions/win32/bin/run_win95.sh &");
        exit(0);
    } else if (access("package/extensions/win32/bin/run_win95.sh", X_OK) == 0) {
        system("/bin/sh package/extensions/win32/bin/run_win95.sh &");
        exit(0);
    } else {
        LOGE("Could not locate run_win95.sh script!");
    }
#endif
}

void desktop_shell_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride) {
    if (!canvas || canvas_w <= 0 || canvas_h <= 0) return;

    /* 1. Desktop Background */
    fill_rect(canvas, canvas_w, canvas_h, stride, 0, 0, canvas_w, canvas_h, 0x80);

    /* 2. When Win32 HLE is running (menu closed):
     * Draw a clean top-right [MENU] button to return to System Selector anytime */
    if (!s_menu_open) {
        int mb_w = 72;
        int mb_h = 28;
        int mb_x = canvas_w - mb_w - 6;
        int mb_y = 6;
        fill_rect(canvas, canvas_w, canvas_h, stride, mb_x, mb_y, mb_w, mb_h, 0xC0);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, mb_x, mb_y, mb_w, mb_h, 0xFF, 0x00);
        draw_3d_rect(canvas, canvas_w, canvas_h, stride, mb_x + 1, mb_y + 1, mb_w - 2, mb_h - 2, 0xDF, 0x80);
        draw_string_scaled(canvas, canvas_w, canvas_h, stride, mb_x + 12, mb_y + 8, "[MENU]", 0x00, 1);
        return;
    }

    /* 3. System Selector Menu (when open) */
    int win_w = 752;
    int win_h = 576;
    if (win_w > canvas_w - 20) win_w = canvas_w - 20;
    if (win_h > canvas_h - 20) win_h = canvas_h - 20;

    int win_x = (canvas_w - win_w) / 2;
    int win_y = (canvas_h - win_h) / 2;

    /* Window Body */
    fill_rect(canvas, canvas_w, canvas_h, stride, win_x, win_y, win_w, win_h, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, win_x, win_y, win_w, win_h, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, win_x + 1, win_y + 1, win_w - 2, win_h - 2, 0xDF, 0x80);

    /* Title Bar */
    fill_rect(canvas, canvas_w, canvas_h, stride, win_x + 3, win_y + 3, win_w - 6, 26, 0x00);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, win_x + 10, win_y + 8,
                       "Microsoft Windows 95 - System Selector", 0xFF, 1);

    /* Center Rotating 3D Windows 95 Logo */
    int logo_cx = canvas_w / 2;
    int logo_cy = win_y + 105;
    win95_logo3d_render(canvas, canvas_w, canvas_h, stride, logo_cx, logo_cy, s_anim_angle, s_anim_wave);

    /* Retro Branding Typography */
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, logo_cx - 64, win_y + 178, "Microsoft", 0x40, 2);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, logo_cx - 100, win_y + 198, "Windows 95", 0x00, 3);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, logo_cx - 144, win_y + 228, "Kindle Paperwhite 5 Retro System", 0x20, 2);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, logo_cx - 132, win_y + 248, "E-Ink Carta 1200 | 1GB RAM | Octa-Core ARM", 0x50, 1);

    /* ------------------------------------------------------------------ */
    /* Button 1: Boot Windows 95                                          */
    /* ------------------------------------------------------------------ */
    int btn_w = win_w - 92;
    if (btn_w > 660) btn_w = 660;
    int btn_x = (canvas_w - btn_w) / 2;
    int btn_h = 56;
    int btn1_y = win_y + 266;

    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn1_y, btn_w, btn_h, 0xD4);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn1_y, btn_w, btn_h, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 1, btn1_y + 1, btn_w - 2, btn_h - 2, 0xEF, 0x70);

    /* Badge 1: [ 95 ] */
    int badge_w = 68;
    int badge_h = 42;
    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn1_y + 7, badge_w, badge_h, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn1_y + 7, badge_w, badge_h, 0x80, 0xFF);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 20, btn1_y + 16, "95", 0xFF, 3);

    /* Text */
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn1_y + 10, "Boot Windows 95 (Full PC OS)", 0x00, 2);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn1_y + 33,
                       "Authentic Windows 95 Desktop & MS-DOS (Tiny386 Hardware PC)", 0x40, 1);

    /* ------------------------------------------------------------------ */
    /* Button 2: Play Minesweeper (Win32 HLE)                             */
    /* ------------------------------------------------------------------ */
    int btn2_y = btn1_y + 64;

    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn2_y, btn_w, btn_h, 0xD4);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn2_y, btn_w, btn_h, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 1, btn2_y + 1, btn_w - 2, btn_h - 2, 0xEF, 0x70);

    /* Badge 2: [ MINE ] */
    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn2_y + 7, badge_w, badge_h, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn2_y + 7, badge_w, badge_h, 0x80, 0xFF);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 12, btn2_y + 19, "MINE", 0xFF, 2);

    /* Text */
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn2_y + 10, "Play Minesweeper (Win32 HLE)", 0x00, 2);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn2_y + 33,
                       "Native x86 execution of classic Minesweeper (Fast & responsive)", 0x40, 1);

    /* ------------------------------------------------------------------ */
    /* Button 3: Play Solitaire (Win32 HLE)                               */
    /* ------------------------------------------------------------------ */
    int btn3_y = btn2_y + 64;

    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn3_y, btn_w, btn_h, 0xD4);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x, btn3_y, btn_w, btn_h, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 1, btn3_y + 1, btn_w - 2, btn_h - 2, 0xEF, 0x70);

    /* Badge 3: [ SOL ] */
    fill_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn3_y + 7, badge_w, badge_h, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, btn_x + 10, btn3_y + 7, badge_w, badge_h, 0x80, 0xFF);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 20, btn3_y + 19, "SOL", 0xFF, 2);

    /* Text */
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn3_y + 10, "Play Solitaire (Win32 HLE)", 0x00, 2);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, btn_x + 88, btn3_y + 33,
                       "Native x86 execution of classic Solitaire with CARDS subsystem", 0x40, 1);

    /* ------------------------------------------------------------------ */
    /* Utility Actions: [ FLASH SCREEN ] and [ EXIT ]                     */
    /* ------------------------------------------------------------------ */
    int btm_y = btn3_y + 72;
    int sub_w = 200;
    int gap = 24;
    int total_sub = sub_w * 2 + gap;
    int sub_x1 = (canvas_w - total_sub) / 2;
    int sub_x2 = sub_x1 + sub_w + gap;

    /* Flash */
    fill_rect(canvas, canvas_w, canvas_h, stride, sub_x1, btm_y, sub_w, 38, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, sub_x1, btm_y, sub_w, 38, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, sub_x1 + 1, btm_y + 1, sub_w - 2, 36, 0xDF, 0x80);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, sub_x1 + 48, btm_y + 13, "FLASH SCREEN", 0x00, 1);

    /* Exit */
    fill_rect(canvas, canvas_w, canvas_h, stride, sub_x2, btm_y, sub_w, 38, 0xC0);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, sub_x2, btm_y, sub_w, 38, 0xFF, 0x00);
    draw_3d_rect(canvas, canvas_w, canvas_h, stride, sub_x2 + 1, btm_y + 1, sub_w - 2, 36, 0xDF, 0x80);
    draw_string_scaled(canvas, canvas_w, canvas_h, stride, sub_x2 + 80, btm_y + 13, "EXIT", 0x00, 1);
}

bool desktop_shell_handle_tap(int x, int y) {
    int canvas_w = DEFAULT_SCREEN_W;
    int canvas_h = DEFAULT_SCREEN_H;
    int stride = 0;
    eink_get_canvas(&canvas_w, &canvas_h, &stride);

    /* 1. If in HLE (menu closed), top-right [MENU] button tap brings menu back */
    if (!s_menu_open) {
        int mb_w = 72;
        int mb_h = 28;
        int mb_x = canvas_w - mb_w - 6;
        int mb_y = 6;
        if (x >= mb_x && x < mb_x + mb_w && y >= mb_y && y < mb_y + mb_h) {
            LOGI("[MENU] button tapped -> Opening System Selector Menu");
            desktop_shell_set_menu_open(true);
            return true;
        }
        return false;
    }

    /* 2. Menu Window Taps (when menu is open) */
    int win_w = 752;
    int win_h = 576;
    if (win_w > canvas_w - 20) win_w = canvas_w - 20;
    if (win_h > canvas_h - 20) win_h = canvas_h - 20;

    int win_y = (canvas_h - win_h) / 2;

    int btn_w = win_w - 92;
    if (btn_w > 660) btn_w = 660;
    int btn_x = (canvas_w - btn_w) / 2;
    int btn_h = 56;

    int btn1_y = win_y + 266;
    int btn2_y = btn1_y + 64;
    int btn3_y = btn2_y + 64;

    /* Tap Button 1: Boot Windows 95 (Full PC OS) */
    if (x >= btn_x && x < btn_x + btn_w && y >= btn1_y && y < btn1_y + btn_h) {
        LOGI("Selected: Boot Windows 95 (Full PC OS)");
        launch_windows_95();
        return true;
    }

    /* Tap Button 2: Play Minesweeper (Win32 HLE) */
    if (x >= btn_x && x < btn_x + btn_w && y >= btn2_y && y < btn2_y + btn_h) {
        LOGI("Selected: Play Minesweeper (Win32 HLE)");
        s_menu_open = false;
        win32_app_request_load("apps/winmine.exe");
        return true;
    }

    /* Tap Button 3: Play Solitaire (Win32 HLE) */
    if (x >= btn_x && x < btn_x + btn_w && y >= btn3_y && y < btn3_y + btn_h) {
        LOGI("Selected: Play Solitaire (Win32 HLE)");
        s_menu_open = false;
        win32_app_request_load("apps/sol.exe");
        return true;
    }

    /* Tap Bottom Actions: FLASH SCREEN and EXIT */
    int btm_y = btn3_y + 72;
    int sub_w = 200;
    int gap = 24;
    int total_sub = sub_w * 2 + gap;
    int sub_x1 = (canvas_w - total_sub) / 2;
    int sub_x2 = sub_x1 + sub_w + gap;

    /* Flash */
    if (x >= sub_x1 && x < sub_x1 + sub_w && y >= btm_y && y < btm_y + 38) {
        LOGI("FLASH SCREEN tapped");
        eink_present(EINK_REFRESH_FULL);
        return true;
    }

    /* Exit */
    if (x >= sub_x2 && x < sub_x2 + sub_w && y >= btm_y && y < btm_y + 38) {
        LOGI("EXIT tapped -> Exiting");
        exit(0);
        return true;
    }

    return true; /* Consume taps inside menu dialog */
}
