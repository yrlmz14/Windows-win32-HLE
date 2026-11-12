#include "user32.h"
#include "gdi32.h"
#include "kernel32.h"
#include "win32_dispatch.h"
#include "../platform/kindle_touch.h"
#include "../platform/eink_render.h"
#include "../platform/vkeyboard.h"
#include <strings.h>
#include <stdlib.h>
#include <string.h>

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

static win32_class_t  s_classes[MAX_CLASSES];
static int            s_class_count = 0;
static win32_window_t s_windows[MAX_WINDOWS];

static WIN_MSG        s_msg_queue[MSG_QUEUE_CAP];
static int            s_msg_head = 0;
static int            s_msg_tail = 0;

static H_HWND         s_active_hwnd = 0;
static H_HWND         s_capture_hwnd = 0;
static WIN_POINT      s_cursor_pos = {100, 100};

static H_HWND         s_open_menu_hwnd = 0;
static int            s_open_menu_idx  = -1;

void user32_init_apis(void);
static LRESULT builtin_wndproc_dispatch(win32_process_t *proc, win32_window_t *w, GUEST_PTR wndproc, UINT msg, WPARAM wParam, LPARAM lParam);
static uint32_t load_menu_internal(win32_process_t *proc, uint32_t menu_id_or_name, bool is_unicode);

static void register_builtin_class(const char *name, GUEST_PTR wndproc, H_HBRUSH brush) {
    if (s_class_count >= MAX_CLASSES) return;
    strncpy(s_classes[s_class_count].class_name, name, sizeof(s_classes[0].class_name) - 1);
    s_classes[s_class_count].wndproc = wndproc;
    s_classes[s_class_count].background_brush = brush;
    s_class_count++;
}

static H_HDC s_desktop_hdc = 0;
static H_HBRUSH s_sys_brushes[32] = {0};

#define MAX_ICONS 64

typedef struct {
    bool in_use;
    int width;
    int height;
    uint32_t pixels[32 * 32]; /* 32x32 ARGB */
} win32_icon_entry_t;

static win32_icon_entry_t s_icons[MAX_ICONS];

#define MAX_MENUS 64
#define MAX_MENU_ITEMS 24

typedef struct {
    char title[48];
    uint32_t id;
    uint32_t flags;
    H_HMENU hsub;
} win32_menu_item_t;

typedef struct {
    bool in_use;
    H_HMENU hmenu;
    int item_count;
    win32_menu_item_t items[MAX_MENU_ITEMS];
} win32_menu_t;

static win32_menu_t s_menus[MAX_MENUS];

void user32_init(void) {
    memset(s_classes, 0, sizeof(s_classes));
    s_class_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (s_windows[i].framebuffer) free(s_windows[i].framebuffer);
        if (s_windows[i].edit_buf) free(s_windows[i].edit_buf);
    }
    memset(s_windows, 0, sizeof(s_windows));
    memset(s_icons, 0, sizeof(s_icons));
    memset(s_menus, 0, sizeof(s_menus));
    memset(s_sys_brushes, 0, sizeof(s_sys_brushes));
    s_desktop_hdc = 0;
    s_msg_head = s_msg_tail = 0;
    s_active_hwnd = 0;
    s_capture_hwnd = 0;
    s_open_menu_hwnd = 0;
    s_open_menu_idx = -1;

    register_builtin_class("Button", BUILTIN_WNDPROC_BUTTON, 16);
    register_builtin_class("Edit", BUILTIN_WNDPROC_EDIT, 6);
    register_builtin_class("Static", BUILTIN_WNDPROC_STATIC, 16);
    register_builtin_class("ListBox", BUILTIN_WNDPROC_LISTBOX, 6);
    register_builtin_class("ComboBox", BUILTIN_WNDPROC_COMBOBOX, 6);
    register_builtin_class("msctls_statusbar32", BUILTIN_WNDPROC_STATUSBAR, 16);
    register_builtin_class("ScrollBar", BUILTIN_WNDPROC_SCROLLBAR, 16);

    user32_init_apis();
}

win32_window_t *user_get_window(H_HWND hwnd) {
    uint32_t idx = (uint32_t)hwnd - 0x4000;
    if (idx > 0 && idx < MAX_WINDOWS && s_windows[idx].in_use) {
        return &s_windows[idx];
    }
    return NULL;
}

H_HWND user_get_active_window(void) {
    return s_active_hwnd;
}

static bool s_windows_dirty = false;

bool user_windows_take_dirty(void) {
    bool d = s_windows_dirty;
    s_windows_dirty = false;
    return d;
}

void user_set_active_window(H_HWND hwnd) {
    if (s_active_hwnd != hwnd) {
        s_windows_dirty = true;
    }
    s_active_hwnd = hwnd;
}



static void get_window_screen_pos(win32_window_t *w, int *out_x, int *out_y) {
    int sx = w->x;
    int sy = w->y;
    H_HWND cur_parent = w->parent;
    while (cur_parent != 0) {
        win32_window_t *pw = user_get_window(cur_parent);
        if (!pw) break;
        sx += pw->x + pw->client_x;
        sy += pw->y + pw->client_y;
        cur_parent = pw->parent;
    }
    *out_x = sx;
    *out_y = sy;
}

bool user_post_message(H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int next = (s_msg_tail + 1) % MSG_QUEUE_CAP;
    if (next == s_msg_head) {
        LOGW("Message queue full, dropping message 0x%04X", msg);
        return false;
    }
    LOGI("user_post_message: HWND 0x%08X, msg 0x%04X, wP=0x%08X, lP=0x%08X", hwnd, msg, (uint32_t)wParam, (uint32_t)lParam);
    s_msg_queue[s_msg_tail].hwnd = hwnd;
    s_msg_queue[s_msg_tail].message = msg;
    s_msg_queue[s_msg_tail].wParam = wParam;
    s_msg_queue[s_msg_tail].lParam = lParam;

    /* In Win32, msg.pt is ALWAYS in screen coordinates.
     * For mouse messages, if lParam carries client (x, y), convert to screen coordinates. */
    if (msg >= WM_MOUSEMOVE && msg <= 0x020E && hwnd) {
        win32_window_t *w = user_get_window(hwnd);
        if (w) {
            int sx = 0, sy = 0;
            get_window_screen_pos(w, &sx, &sy);
            int cl_x = (int16_t)(lParam & 0xFFFF);
            int cl_y = (int16_t)((lParam >> 16) & 0xFFFF);
            s_msg_queue[s_msg_tail].pt.x = sx + w->client_x + cl_x;
            s_msg_queue[s_msg_tail].pt.y = sy + w->client_y + cl_y;
            s_cursor_pos = s_msg_queue[s_msg_tail].pt;
        } else {
            s_msg_queue[s_msg_tail].pt = s_cursor_pos;
        }
    } else {
        s_msg_queue[s_msg_tail].pt = s_cursor_pos;
    }
    s_msg_tail = next;
    return true;
}

/* Call Window Procedure in guest x86 space or built-in HLE class handler */
static LRESULT call_wndproc(win32_process_t *proc, GUEST_PTR wndproc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    win32_window_t *w = user_get_window(hwnd);
    if (wndproc >= 0xFE010000U && wndproc <= 0xFE0100FFU) {
        return builtin_wndproc_dispatch(proc, w, wndproc, msg, wParam, lParam);
    }
    if (!proc || !wndproc) return 0;

    if (msg != 0 && msg != WM_PAINT && msg != 0x0014 && msg != 0x0085 && msg != 0x0200) {
        LOGI("call_wndproc: HWND 0x%08X, msg 0x%04X (wP=0x%08X, lP=0x%08X), wndproc=0x%08X",
             hwnd, msg, (uint32_t)wParam, (uint32_t)lParam, wndproc);
    }

    /* Push arguments for __stdcall WndProc(HWND, UINT, WPARAM, LPARAM) */
    uint32_t saved_esp = proc->cpu.regs.esp;
    uint32_t saved_eip = proc->cpu.regs.eip;

    proc->cpu.regs.esp -= 20;
    uint32_t stack_args[5] = {
        0xFFFFFFFE, /* Synthetic return address */
        (uint32_t)hwnd,
        (uint32_t)msg,
        (uint32_t)wParam,
        (uint32_t)lParam
    };
    if (!guest_mem_write(&proc->mem, proc->cpu.regs.esp, stack_args, sizeof(stack_args))) return 0;

    proc->cpu.regs.eip = wndproc;

    /* Run instructions until return */
    int steps = 0;
    while (proc->cpu.running && proc->cpu.regs.eip != 0xFFFFFFFE && steps < 20000000) {
        if ((proc->cpu.regs.eip & HLE_API_MASK) == HLE_API_BASE) {
            uint32_t api_id = proc->cpu.regs.eip & ~HLE_API_MASK;
            win32_dispatch_api(proc, api_id);
            continue;
        }
        x86_step(&proc->cpu);
        steps++;
    }

    LRESULT result = (LRESULT)proc->cpu.regs.eax;
    if (steps >= 20000000) {
        LOGW("call_wndproc: execution step limit (20,000,000 steps) reached for msg 0x%04X", msg);
    } else if (msg != 0 && msg != WM_PAINT && msg != 0x0014 && msg != 0x0085 && msg != 0x0200) {
        LOGI("call_wndproc: msg 0x%04X done in %d steps, ret=0x%08X", msg, steps, (uint32_t)result);
    }
    proc->cpu.regs.esp = saved_esp;
    proc->cpu.regs.eip = saved_eip;
    return result;
}

bool user_send_message(win32_process_t *proc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result) {
    win32_window_t *w = user_get_window(hwnd);
    if (!w) return false;
    LRESULT res = call_wndproc(proc, w->wndproc, hwnd, msg, wParam, lParam);
    if (result) *result = res;
    return true;
}

/* API Handlers */
static uint32_t u32_RegisterClassA(win32_process_t *proc, uint32_t *args) {
    WIN_WNDCLASS *wc = (WIN_WNDCLASS *)guest_va_to_host(&proc->mem, args[0]);
    if (!wc) return 0;

    const char *name = (const char *)guest_va_to_host(&proc->mem, wc->lpszClassName);
    if (!name || s_class_count >= MAX_CLASSES) return 0;

    strncpy(s_classes[s_class_count].class_name, name, sizeof(s_classes[0].class_name) - 1);
    s_classes[s_class_count].wndproc = wc->lpfnWndProc;
    s_classes[s_class_count].icon = wc->hIcon;
    s_classes[s_class_count].cursor = wc->hCursor;
    s_classes[s_class_count].background_brush = wc->hbrBackground;
    s_classes[s_class_count].menu_id = wc->lpszMenuName;
    if ((wc->lpszMenuName >> 16) != 0) {
        const char *mname = (const char *)guest_va_to_host(&proc->mem, wc->lpszMenuName);
        if (mname) strncpy(s_classes[s_class_count].menu_name, mname, sizeof(s_classes[0].menu_name) - 1);
    }

    LOGI("RegisterClassA: %s (WndProc=0x%08X, menu=0x%X)", name, wc->lpfnWndProc, wc->lpszMenuName);
    s_class_count++;
    return 0x0001; /* Atom */
}

/* 32-bit ARGB Window Decoration & Font Rendering Engine */
static void draw_text_argb(uint8_t *fb, int stride, int fb_w, int fb_h, int x, int y, const char *str, uint32_t color) {
    if (!fb || !str) return;
    int len = (int)strlen(str);
    for (int i = 0; i < len; i++) {
        char ch = str[i];
        if (ch < 32 || ch > 126) ch = ' ';
        const uint8_t *glyph = GDI_FONT_8X8[ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= fb_h) continue;
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                int px = x + i * 8 + gx;
                if (px < 0 || px >= fb_w) continue;
                if (row & (0x80 >> gx)) {
                    *(uint32_t *)(fb + py * stride + px * 4) = color;
                }
            }
        }
    }
}

static void fill_rect_argb(uint8_t *fb, int stride, int fb_w, int fb_h, int x, int y, int w, int h, uint32_t color) {
    if (!fb) return;
    for (int dy = y; dy < y + h; dy++) {
        if (dy < 0 || dy >= fb_h) continue;
        for (int dx = x; dx < x + w; dx++) {
            if (dx < 0 || dx >= fb_w) continue;
            *(uint32_t *)(fb + dy * stride + dx * 4) = color;
        }
    }
}

static void draw_3d_rect_argb(uint8_t *fb, int stride, int fb_w, int fb_h, int x, int y, int w, int h, uint32_t c_tl, uint32_t c_br) {
    if (!fb) return;
    for (int dx = x; dx < x + w; dx++) {
        if (dx >= 0 && dx < fb_w && y >= 0 && y < fb_h)
            *(uint32_t *)(fb + y * stride + dx * 4) = c_tl;
        int by = y + h - 1;
        if (dx >= 0 && dx < fb_w && by >= 0 && by < fb_h)
            *(uint32_t *)(fb + by * stride + dx * 4) = c_br;
    }
    for (int dy = y; dy < y + h; dy++) {
        if (x >= 0 && x < fb_w && dy >= 0 && dy < fb_h)
            *(uint32_t *)(fb + dy * stride + x * 4) = c_tl;
        int rx = x + w - 1;
        if (rx >= 0 && rx < fb_w && dy >= 0 && dy < fb_h)
            *(uint32_t *)(fb + dy * stride + rx * 4) = c_br;
    }
}

static void draw_button_argb(uint8_t *fb, int stride, int fb_w, int fb_h, int x, int y, int w, int h, char type) {
    fill_rect_argb(fb, stride, fb_w, fb_h, x, y, w, h, 0xFFC0C0C0);
    draw_3d_rect_argb(fb, stride, fb_w, fb_h, x, y, w, h, 0xFFFFFFFF, 0xFF000000);
    draw_3d_rect_argb(fb, stride, fb_w, fb_h, x + 1, y + 1, w - 2, h - 2, 0xFFDFDFDF, 0xFF808080);
    if (type == 'X') {
        for (int i = 0; i < 6; i++) {
            int cx1 = x + 4 + i, cy1 = y + 4 + i;
            int cx2 = x + 9 - i, cy2 = y + 4 + i;
            if (cx1 >= 0 && cx1 < fb_w && cy1 >= 0 && cy1 < fb_h)
                *(uint32_t *)(fb + cy1 * stride + cx1 * 4) = 0xFF000000;
            if (cx2 >= 0 && cx2 < fb_w && cy2 >= 0 && cy2 < fb_h)
                *(uint32_t *)(fb + cy2 * stride + cx2 * 4) = 0xFF000000;
        }
    } else if (type == '_') {
        for (int i = 0; i < 6; i++) {
            int px = x + 4 + i, py = y + 9;
            if (px >= 0 && px < fb_w && py >= 0 && py < fb_h) {
                *(uint32_t *)(fb + py * stride + px * 4) = 0xFF000000;
                *(uint32_t *)(fb + (py - 1) * stride + px * 4) = 0xFF000000;
            }
        }
    } else if (type == '[') {
        draw_3d_rect_argb(fb, stride, fb_w, fb_h, x + 3, y + 3, 8, 8, 0xFF000000, 0xFF000000);
        for (int i = 0; i < 8; i++) {
            if (x + 3 + i < fb_w && y + 4 < fb_h)
                *(uint32_t *)(fb + (y + 4) * stride + (x + 3 + i) * 4) = 0xFF000000;
        }
    }
}
H_HICON user_extract_icon(const char *exe_path, int icon_index) {
    if (!exe_path) return 0;

    int slot = -1;
    for (int i = 1; i < MAX_ICONS; i++) {
        if (!s_icons[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return 0;

    uint32_t argb[32 * 32];
    if (!pe_extract_icon_file(exe_path, icon_index, argb)) {
        LOGW("user_extract_icon: Could not extract icon from %s, using fallback", exe_path);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                uint32_t c = 0x00000000;
                if (x >= 4 && x <= 27 && y >= 2 && y <= 29) {
                    c = 0xFFFFFFFF;
                    if (x == 4 || x == 27 || y == 2 || y == 29) c = 0xFF000000;
                    else if (y <= 7) c = 0xFF000080;
                }
                argb[y * 32 + x] = c;
            }
        }
    }

    s_icons[slot].in_use = true;
    s_icons[slot].width = 32;
    s_icons[slot].height = 32;
    memcpy(s_icons[slot].pixels, argb, sizeof(argb));
    LOGI("user_extract_icon: Loaded icon for %s -> HICON 0x%04X", exe_path, slot);
    return (H_HICON)slot;
}

static uint8_t *allocate_window_framebuffer(int w, int h, uint32_t bg_color) {
    if (w <= 0 || h <= 0) return NULL;
    uint32_t *fb = (uint32_t *)malloc(w * h * sizeof(uint32_t));
    if (!fb) return NULL;
    for (int i = 0; i < w * h; i++) {
        fb[i] = bg_color;
    }
    return (uint8_t *)fb;
}

static void update_window_client_area(win32_window_t *w) {
    if (!w) return;
    bool has_caption = (w->style & WS_CAPTION) == WS_CAPTION;
    bool has_border = (w->style & (WS_BORDER | WS_THICKFRAME | WS_DLGFRAME)) != 0;

    int border = has_border ? 3 : 0;
    int cap_h = has_caption ? 19 : 0;
    int menu_h = (w->menu != 0 && w->height > 50) ? 20 : 0;

    w->client_x = border;
    w->client_y = border + cap_h + menu_h;
    w->client_w = w->width - border * 2;
    w->client_h = w->height - w->client_y - border;
    if (w->client_w < 0) w->client_w = 0;
    if (w->client_h < 0) w->client_h = 0;

    if (w->hdc && w->framebuffer) {
        gdi_dc_t *dc = gdi_get_dc(w->hdc);
        if (dc) {
            gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
            if (bm) {
                bm->bitmap.width = w->client_w;
                bm->bitmap.height = w->client_h;
                bm->bitmap.stride = w->fb_stride;
                bm->bitmap.bits = w->framebuffer + (w->client_y * w->fb_stride) + (w->client_x * 4);
                bm->bitmap.is_dib = true;
                bm->bitmap.is_window_fb = true; /* Prevent DeleteObject/gdi32_init from freeing window->framebuffer */
            }
        }
    }
}

static void paint_window_decorations(win32_window_t *w) {
    if (!w || !w->framebuffer || w->width <= 0 || w->height <= 0) return;
    int bw = w->width;
    int bh = w->height;
    int stride = w->fb_stride;
    uint8_t *fb = w->framebuffer;

    bool is_active = (w->hwnd == s_active_hwnd);
    bool is_child = (w->style & WS_CHILD) != 0;
    bool has_caption = (w->style & WS_CAPTION) == WS_CAPTION;
    bool has_border = (w->style & (WS_BORDER | WS_THICKFRAME | WS_DLGFRAME)) != 0;

    /* 2. Window Borders */
    if (has_border) {
        draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFFFFFFF, 0xFF000000);
        if (bw > 4 && bh > 4) {
            draw_3d_rect_argb(fb, stride, bw, bh, 1, 1, bw - 2, bh - 2, 0xFFDFDFDF, 0xFF808080);
        }
    }

    /* 3. Title / Caption Bar */
    if (has_caption && bw > 30 && bh > 25) {
        int cap_x = 3;
        int cap_y = 3;
        int cap_w = bw - 6;
        int cap_h = 18;

        uint32_t cap_bg = is_active ? 0xFF000080 : 0xFF808080;
        fill_rect_argb(fb, stride, bw, bh, cap_x, cap_y, cap_w, cap_h, cap_bg);

        /* System menu / Icon box on left */
        if (w->style & WS_SYSMENU) {
            fill_rect_argb(fb, stride, bw, bh, cap_x + 2, cap_y + 2, 14, 14, 0xFFC0C0C0);
            draw_3d_rect_argb(fb, stride, bw, bh, cap_x + 2, cap_y + 2, 14, 14, 0xFFFFFFFF, 0xFF000000);
            fill_rect_argb(fb, stride, bw, bh, cap_x + 5, cap_y + 7, 8, 4, 0xFF000080);
        }

        /* Title text */
        int title_x = (w->style & WS_SYSMENU) ? (cap_x + 20) : (cap_x + 6);
        if (w->title[0]) {
            draw_text_argb(fb, stride, bw, bh, title_x, cap_y + 5, w->title, 0xFFFFFFFF);
        }

        /* Minimize / Maximize / Close Buttons on right */
        int btn_right = cap_x + cap_w - 2;
        if (w->style & WS_SYSMENU) {
            draw_button_argb(fb, stride, bw, bh, btn_right - 14, cap_y + 2, 14, 14, 'X');
            btn_right -= 16;
        }
        if (w->style & WS_MAXIMIZEBOX) {
            draw_button_argb(fb, stride, bw, bh, btn_right - 14, cap_y + 2, 14, 14, '[');
            btn_right -= 16;
        }
        if (w->style & WS_MINIMIZEBOX) {
            draw_button_argb(fb, stride, bw, bh, btn_right - 14, cap_y + 2, 14, 14, '_');
            btn_right -= 16;
        }
    }

    /* 4. Menu Bar */
    if (!is_child && w->menu != 0 && bh > 50) {
        int menu_y = 23;
        int menu_h = 20;
        fill_rect_argb(fb, stride, bw, bh, 3, menu_y, bw - 6, menu_h, 0xFFC0C0C0);
        for (int x = 3; x < bw - 3; x++) {
            *(uint32_t *)(fb + (menu_y + menu_h - 2) * stride + x * 4) = 0xFF808080;
            *(uint32_t *)(fb + (menu_y + menu_h - 1) * stride + x * 4) = 0xFFFFFFFF;
        }

        int cur_x = 10;
        int m_slot = (int)(w->menu - 0x5000);
        if (m_slot >= 1 && m_slot < MAX_MENUS && s_menus[m_slot].in_use) {
            win32_menu_t *m = &s_menus[m_slot];
            for (int i = 0; i < m->item_count; i++) {
                if (m->items[i].title[0]) {
                    draw_text_argb(fb, stride, bw, bh, cur_x, menu_y + 4, m->items[i].title, 0xFF000000);
                    cur_x += (int)strlen(m->items[i].title) * 8 + 14;
                }
            }
        }
    }
}

static uint32_t u32_CreateWindowExA(win32_process_t *proc, uint32_t *args) {
    uint32_t ex_style = args[0];
    const char *class_name = (const char *)guest_va_to_host(&proc->mem, args[1]);
    const char *title = (const char *)guest_va_to_host(&proc->mem, args[2]);
    uint32_t style = args[3];
    int x = (int)args[4];
    int y = (int)args[5];
    int w = (int)args[6];
    int h = (int)args[7];
    H_HWND parent = (H_HWND)args[8];
    H_HMENU menu = (H_HMENU)args[9];
    /* H_HINSTANCE inst = (H_HINSTANCE)args[10]; */

    /* Find available window slot */
    int slot = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return 0;

    H_HWND hwnd = (H_HWND)(0x4000 + slot);
    win32_window_t *win = &s_windows[slot];
    memset(win, 0, sizeof(win32_window_t));
    win->in_use = true;
    win->hwnd = hwnd;
    win->parent = parent;
    win->style = style;
    win->ex_style = ex_style;
    win->menu = menu;

    if (class_name) strncpy(win->class_name, class_name, sizeof(win->class_name) - 1);
    if (title) strncpy(win->title, title, sizeof(win->title) - 1);

    /* Lookup class wndproc and default menu */
    for (int i = 0; i < s_class_count; i++) {
        if (strcasecmp(s_classes[i].class_name, win->class_name) == 0) {
            win->wndproc = s_classes[i].wndproc;
            if (!(style & WS_CHILD) && win->menu == 0 && s_classes[i].menu_id != 0) {
                win->menu = (H_HMENU)load_menu_internal(proc, s_classes[i].menu_id, false);
            }
            break;
        }
    }

    /* Adjust position if CW_USEDEFAULT (0x80000000) */
    if (w <= 0 || w == (int)0x80000000) w = 640;
    if (h <= 0 || h == (int)0x80000000) h = 480;
    if (x == (int)0x80000000) x = (DEFAULT_SCREEN_W > w) ? (DEFAULT_SCREEN_W - w) / 2 : 40;
    if (y == (int)0x80000000) y = (DEFAULT_SCREEN_H > h) ? (DEFAULT_SCREEN_H - h) / 2 : 40;

    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->visible = (style & WS_VISIBLE) != 0;
    win->enabled = true;

    /* Allocate Window DC and Framebuffer surface */
    win->hdc = gdi_create_window_dc(hwnd);
    win->fb_stride = w * 4;
    uint32_t bg_col = (strcasecmp(win->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
    win->framebuffer = allocate_window_framebuffer(w, h, bg_col);

    /* Point window DC's default bitmap to window client area */
    gdi_dc_t *dc = gdi_get_dc(win->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        bm_obj->bitmap.bpp = 32;
        dc->selected_bitmap = (H_HBITMAP)hbm;
    }
    update_window_client_area(win);
    paint_window_decorations(win);

    s_active_hwnd = (style & WS_CHILD) ? (s_active_hwnd ? s_active_hwnd : parent) : hwnd;
    LOGI("CreateWindowExA: '%s' (%s) -> HWND 0x%08X [%d,%d %dx%d]",
         win->title, win->class_name, hwnd, x, y, w, h);

    /* Send WM_CREATE and WM_SIZE */
    user_send_message(proc, hwnd, WM_CREATE, 0, 0, NULL);
    user_send_message(proc, hwnd, WM_SIZE, 0, ((uint32_t)win->client_h << 16) | (win->client_w & 0xFFFF), NULL);

    return (uint32_t)hwnd;
}

static uint32_t u32_ShowWindow(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    int cmd = (int)args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        w->visible = (cmd != SW_HIDE);
        w->needs_redraw = true;
        s_windows_dirty = true;
        if (!(w->style & WS_CHILD)) s_active_hwnd = hwnd;
        if (w->visible) {
            user_send_message(proc, hwnd, WM_SIZE, 0, ((uint32_t)w->client_h << 16) | (w->client_w & 0xFFFF), NULL);
        }
        user_post_message(hwnd, WM_PAINT, 0, 0);
        return 1;
    }
    return 0;
}

static uint32_t u32_UpdateWindow(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w && w->visible) {
        s_windows_dirty = true;
        /* Send WM_PAINT */
        user_send_message(proc, hwnd, WM_PAINT, 0, 0, NULL);
        return 1;
    }
    return 0;
}

static uint32_t u32_DestroyWindow(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        user_send_message(proc, hwnd, WM_DESTROY, 0, 0, NULL);
        if (w->framebuffer) { free(w->framebuffer); w->framebuffer = NULL; }
        w->in_use = false;
        if (s_active_hwnd == hwnd) s_active_hwnd = 0;
        return 1;
    }
    return 0;
}

static uint32_t u32_GetMessageA(win32_process_t *proc, uint32_t *args) {
    uint32_t msg_va = args[0];
    WIN_MSG *out_msg = (WIN_MSG *)guest_va_to_host(&proc->mem, msg_va);
    if (!out_msg) return 0;

    /* If queue is empty, poll hardware touch inputs */
    if (s_msg_head == s_msg_tail) {
        touch_poll();
    }

    /* Check if queue has a message */
    if (s_msg_head != s_msg_tail) {
        *out_msg = s_msg_queue[s_msg_head];
        s_msg_head = (s_msg_head + 1) % MSG_QUEUE_CAP;
        return (out_msg->message != WM_QUIT);
    }

    /* Check if any window needs WM_PAINT */
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (s_windows[i].in_use && s_windows[i].visible && s_windows[i].needs_redraw) {
            s_windows[i].needs_redraw = false;
            out_msg->hwnd = s_windows[i].hwnd;
            out_msg->message = WM_PAINT;
            out_msg->wParam = 0;
            out_msg->lParam = 0;
            out_msg->time = 0;
            out_msg->pt = s_cursor_pos;
            return 1;
        }
    }

    /* No messages queued: return WM_NULL and yield instruction loop */
    if (proc) proc->cpu.yielded = true;
    memset(out_msg, 0, sizeof(WIN_MSG));
    out_msg->message = 0; /* WM_NULL */
    return 1;
}

static uint32_t u32_PostMessage(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    UINT msg = args[1];
    WPARAM wParam = args[2];
    LPARAM lParam = args[3];
    LOGI("u32_PostMessage: HWND 0x%08X, msg 0x%04X, wP=0x%08X, lP=0x%08X", hwnd, msg, wParam, lParam);
    return user_post_message(hwnd, msg, wParam, lParam) ? 1 : 0;
}

static uint32_t u32_PeekMessageA(win32_process_t *proc, uint32_t *args) {
    uint32_t msg_va = args[0];
    uint32_t filter_min = args[2];
    uint32_t filter_max = args[3];
    uint32_t remove = args[4];

    WIN_MSG *out_msg = (WIN_MSG *)guest_va_to_host(&proc->mem, msg_va);
    if (!out_msg) return 0;

    /* Always poll hardware touch/mouse inputs so modal loops (e.g. smiley button) receive move/up events! */
    touch_poll();
    if (touch_take_dirty()) {
        int canvas_w, canvas_h, canvas_stride;
        uint8_t *canvas = eink_get_canvas(&canvas_w, &canvas_h, &canvas_stride);
        eink_clear(EINK_LIGHT_GRAY);
        user_composite_windows(canvas, canvas_w, canvas_h, canvas_stride);
        if (vkeyboard_is_visible()) {
            vkeyboard_draw(canvas, canvas_w, canvas_h, canvas_stride);
        }
        eink_present(EINK_REFRESH_FAST);
    }

    /* Search queue for a message matching filter */
    int found_idx = -1;
    int curr = s_msg_head;
    while (curr != s_msg_tail) {
        UINT m = s_msg_queue[curr].message;
        bool match = true;
        if (filter_min != 0 && m < filter_min) match = false;
        if (filter_max != 0 && m > filter_max) match = false;
        if (match) {
            found_idx = curr;
            break;
        }
        curr = (curr + 1) % MSG_QUEUE_CAP;
    }

    if (found_idx >= 0) {
        *out_msg = s_msg_queue[found_idx];
        if (remove & 1) {
            if (found_idx == s_msg_head) {
                s_msg_head = (s_msg_head + 1) % MSG_QUEUE_CAP;
            } else {
                int i = found_idx;
                while (i != s_msg_tail) {
                    int next = (i + 1) % MSG_QUEUE_CAP;
                    if (next == s_msg_tail) break;
                    s_msg_queue[i] = s_msg_queue[next];
                    i = next;
                }
                s_msg_tail = (s_msg_tail - 1 + MSG_QUEUE_CAP) % MSG_QUEUE_CAP;
            }
        }
        return 1;
    }

    /* Check if any window needs WM_PAINT (only if filter allows WM_PAINT = 0x000F) */
    bool paint_allowed = (filter_min == 0 || (filter_min <= WM_PAINT && filter_max >= WM_PAINT));
    if (paint_allowed) {
        for (int i = 1; i < MAX_WINDOWS; i++) {
            if (s_windows[i].in_use && s_windows[i].visible && s_windows[i].needs_redraw) {
                if (remove & 1) {
                    s_windows[i].needs_redraw = false;
                }
                out_msg->hwnd = s_windows[i].hwnd;
                out_msg->message = WM_PAINT;
                out_msg->wParam = 0;
                out_msg->lParam = 0;
                out_msg->time = 0;
                out_msg->pt = s_cursor_pos;
                return 1;
            }
        }
    }

    /* No message matching: sleep briefly in modal loops to yield CPU, and yield batch */
    sys_sleep_ms(2);
    if (proc) proc->cpu.yielded = true;
    return 0;
}

static uint32_t u32_TranslateMessage(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_DispatchMessageA(win32_process_t *proc, uint32_t *args) {
    uint32_t msg_va = args[0];
    WIN_MSG *msg = (WIN_MSG *)guest_va_to_host(&proc->mem, msg_va);
    if (!msg) return 0;

    win32_window_t *w = user_get_window(msg->hwnd);
    if (w) {
        LRESULT res = 0;
        user_send_message(proc, msg->hwnd, msg->message, msg->wParam, msg->lParam, &res);
        return (uint32_t)res;
    }
    return 0;
}

static uint32_t u32_PostQuitMessage(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    user_post_message(s_active_hwnd, WM_QUIT, args[0], 0);
    return 0;
}

static uint32_t u32_DefWindowProcA(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    UINT msg = args[1];
    /* WPARAM wParam = args[2]; */
    LPARAM lParam = args[3];

    if (msg == WM_CLOSE) {
        u32_DestroyWindow(proc, args);
        return 0;
    }
    if (msg == WM_DESTROY) {
        user_post_message(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    if (msg == 0x0203 /* WM_LBUTTONDBLCLK */) {
        user_post_message(hwnd, 0x00A3 /* WM_NCLBUTTONDBLCLK */, 2 /* HTCAPTION */, lParam);
        return 0;
    }
    if (msg == 0x0014 /* WM_ERASEBKGND */) {
        win32_window_t *w = user_get_window(hwnd);
        if (w && w->framebuffer) {
            paint_window_decorations(w);
            H_HBRUSH brush = 0;
            for (int i = 0; i < s_class_count; i++) {
                if (strcmp(s_classes[i].class_name, w->class_name) == 0) {
                    brush = s_classes[i].background_brush;
                    break;
                }
            }
            if (brush) {
                gdi_object_t *br_obj = gdi_get_obj((H_HANDLE)brush);
                if (br_obj && br_obj->type == GDI_OBJ_BRUSH) {
                    uint32_t color = br_obj->brush.color | 0xFF000000;
                    fill_rect_argb(w->framebuffer, w->fb_stride, w->width, w->height,
                                   w->client_x, w->client_y, w->client_w, w->client_h, color);
                }
            }
        }
        return 1;
    }
    if (msg == WM_PAINT || msg == 0x0085 /* WM_NCPAINT */) {
        win32_window_t *w = user_get_window(hwnd);
        if (w) paint_window_decorations(w);
        return 0;
    }
    return 0;
}

static uint32_t u32_BeginPaint(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    WIN_PAINTSTRUCT *ps = (WIN_PAINTSTRUCT *)guest_va_to_host(&proc->mem, args[1]);
    win32_window_t *w = user_get_window(hwnd);
    if (w && ps) {
        ps->hdc = w->hdc;
        ps->rcPaint.left = 0;
        ps->rcPaint.top = 0;
        ps->rcPaint.right = w->width;
        ps->rcPaint.bottom = w->height;
        return (uint32_t)w->hdc;
    }
    return 0;
}

static uint32_t u32_EndPaint(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    s_windows_dirty = true;
    return 1;
}

static uint32_t u32_GetDC(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    if (hwnd == 0) {
        if (!s_desktop_hdc) {
            s_desktop_hdc = gdi_create_window_dc(0);
        }
        return (uint32_t)s_desktop_hdc;
    }
    win32_window_t *w = user_get_window(hwnd);
    if (w) return (uint32_t)w->hdc;
    return 0;
}

static uint32_t u32_ReleaseDC(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    s_windows_dirty = true;
    return 1;
}

static uint32_t u32_MessageBoxA(win32_process_t *proc, uint32_t *args) {
    const char *text = (const char *)guest_va_to_host(&proc->mem, args[1]);
    const char *caption = (const char *)guest_va_to_host(&proc->mem, args[2]);
    LOGI(">>> MessageBoxA: [%s] %s <<<", caption ? caption : "Message", text ? text : "");
    return 1; /* IDOK */
}



/* Touch & Mouse Event Injection */
void user_inject_mouse_down(int screen_x, int screen_y, bool right_button) {
    s_cursor_pos.x = screen_x;
    s_cursor_pos.y = screen_y;

    /* 0. If a popup dropdown menu is currently open, check if the click hits it */
    if (s_open_menu_hwnd != 0 && s_open_menu_idx >= 0) {
        win32_window_t *mw = user_get_window(s_open_menu_hwnd);
        if (mw && mw->menu != 0) {
            int m_slot = (int)(mw->menu - 0x5000);
            if (m_slot >= 1 && m_slot < MAX_MENUS && s_menus[m_slot].in_use) {
                win32_menu_t *m = &s_menus[m_slot];
                if (s_open_menu_idx < m->item_count && m->items[s_open_menu_idx].hsub != 0) {
                    int sub_slot = (int)(m->items[s_open_menu_idx].hsub - 0x5000);
                    if (sub_slot >= 1 && sub_slot < MAX_MENUS && s_menus[sub_slot].in_use) {
                        win32_menu_t *sub = &s_menus[sub_slot];
                        int sx = 0, sy = 0;
                        get_window_screen_pos(mw, &sx, &sy);
                        int drop_x = sx + 10;
                        for (int k = 0; k < s_open_menu_idx; k++) {
                            drop_x += (int)strlen(m->items[k].title) * 8 + 14;
                        }
                        int drop_y = sy + 43;
                        int drop_w = 180;
                        for (int idx = 0; idx < sub->item_count; idx++) {
                            int iw = (int)strlen(sub->items[idx].title) * 8 + 20;
                            if (iw > drop_w) drop_w = iw;
                        }
                        int drop_h = sub->item_count * 18 + 6;
                        if (screen_x >= drop_x && screen_x < drop_x + drop_w &&
                            screen_y >= drop_y && screen_y < drop_y + drop_h) {
                            int idx = (screen_y - (drop_y + 3)) / 18;
                            if (idx >= 0 && idx < sub->item_count) {
                                uint32_t cmd_id = sub->items[idx].id;
                                s_open_menu_hwnd = 0;
                                s_open_menu_idx = -1;
                                s_windows_dirty = true;
                                if (cmd_id != 0 && !(sub->items[idx].flags & 0x0800)) {
                                    user_post_message(mw->hwnd, WM_COMMAND, cmd_id, 0);
                                }
                                return;
                            }
                        }
                    }
                }
            }
        }
        s_open_menu_hwnd = 0;
        s_open_menu_idx = -1;
        s_windows_dirty = true;
    }

    /* Find top-most window hit (children before parents) */
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        if (!s_windows[i].in_use || !s_windows[i].visible) continue;
        win32_window_t *w = &s_windows[i];
        /* Static labels without SS_NOTIFY are transparent to mouse hits */
        if (w->wndproc == BUILTIN_WNDPROC_STATIC && !(w->style & 0x0100 /* SS_NOTIFY */)) continue;
        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);
        if (screen_x >= sx && screen_x < sx + w->width &&
            screen_y >= sy && screen_y < sy + w->height) {
            s_active_hwnd = w->hwnd;
            w->needs_redraw = true;
            paint_window_decorations(w);
            int rel_x = screen_x - sx;
            int rel_y = screen_y - sy;
            LPARAM lp = (uint32_t)(rel_y << 16) | (uint16_t)rel_x;

            bool is_child = (w->style & WS_CHILD) != 0;
            bool has_caption = (w->style & WS_CAPTION) == WS_CAPTION;

            if (has_caption && rel_y < 23) {
                /* Check Close ('X') button hit */
                int btn_right = w->width - 5;
                if ((w->style & WS_SYSMENU) && rel_x >= btn_right - 14 && rel_x <= btn_right) {
                    user_post_message(w->hwnd, WM_CLOSE, 0, 0);
                    return;
                }
                UINT msg = right_button ? 0x00A4 /* WM_NCRBUTTONDOWN */ : 0x00A1 /* WM_NCLBUTTONDOWN */;
                user_post_message(w->hwnd, msg, 2 /* HTCAPTION */, lp);
            } else if (!is_child && w->menu != 0 && rel_y >= 23 && rel_y < w->client_y) {
                /* Menu bar item clicked */
                int cur_x = 10;
                int m_slot = (int)(w->menu - 0x5000);
                if (m_slot >= 1 && m_slot < MAX_MENUS && s_menus[m_slot].in_use) {
                    win32_menu_t *m = &s_menus[m_slot];
                    for (int mi = 0; mi < m->item_count; mi++) {
                        int item_w = (int)strlen(m->items[mi].title) * 8 + 14;
                        if (rel_x >= cur_x && rel_x < cur_x + item_w) {
                            if (m->items[mi].hsub != 0) {
                                s_open_menu_hwnd = w->hwnd;
                                s_open_menu_idx = mi;
                                s_windows_dirty = true;
                                user_post_message(w->hwnd, 0x0117 /* WM_INITMENUPOPUP */, (WPARAM)m->items[mi].hsub, mi);
                            } else if (m->items[mi].id != 0) {
                                user_post_message(w->hwnd, WM_COMMAND, m->items[mi].id, 0);
                            }
                            break;
                        }
                        cur_x += item_w;
                    }
                }
            } else {
                int cl_x = rel_x - w->client_x;
                int cl_y = rel_y - w->client_y;
                LPARAM client_lp = (uint32_t)(cl_y << 16) | (uint16_t)cl_x;
                UINT msg = right_button ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
                user_post_message(w->hwnd, msg, right_button ? 2 : 1, client_lp);
            }
            return;
        }
    }
}

void user_inject_mouse_up(int screen_x, int screen_y, bool right_button) {
    s_cursor_pos.x = screen_x;
    s_cursor_pos.y = screen_y;

    H_HWND target = s_capture_hwnd ? s_capture_hwnd : s_active_hwnd;
    win32_window_t *w = user_get_window(target);
    if (w) {
        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);
        int rel_x = screen_x - sx;
        int rel_y = screen_y - sy;
        int cl_x = rel_x - w->client_x;
        int cl_y = rel_y - w->client_y;
        LPARAM lp = (uint32_t)(cl_y << 16) | (uint16_t)cl_x;
        UINT msg = right_button ? WM_RBUTTONUP : WM_LBUTTONUP;
        user_post_message(w->hwnd, msg, 0, lp);
    }
}

void user_inject_mouse_dblclk(int screen_x, int screen_y) {
    s_cursor_pos.x = screen_x;
    s_cursor_pos.y = screen_y;

    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        if (!s_windows[i].in_use || !s_windows[i].visible) continue;
        win32_window_t *w = &s_windows[i];
        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);
        if (screen_x >= sx && screen_x < sx + w->width &&
            screen_y >= sy && screen_y < sy + w->height) {
            s_active_hwnd = w->hwnd;
            int rel_x = screen_x - sx;
            int rel_y = screen_y - sy;
            LPARAM lp = (uint32_t)(rel_y << 16) | (uint16_t)rel_x;

            bool is_child = (w->style & WS_CHILD) != 0;
            (void)is_child;
            bool has_caption = (w->style & WS_CAPTION) == WS_CAPTION;

            if (has_caption && rel_y < 22) {
                user_post_message(w->hwnd, 0x00A3 /* WM_NCLBUTTONDBLCLK */, 2 /* HTCAPTION */, lp);
            } else {
                int cl_x = rel_x - w->client_x;
                int cl_y = rel_y - w->client_y;
                LPARAM client_lp = (uint32_t)(cl_y << 16) | (uint16_t)cl_x;
                user_post_message(w->hwnd, 0x0203 /* WM_LBUTTONDBLCLK */, 1, client_lp);
            }
            return;
        }
    }
}

void user_inject_mouse_move(int screen_x, int screen_y) {
    s_cursor_pos.x = screen_x;
    s_cursor_pos.y = screen_y;

    H_HWND target = s_capture_hwnd ? s_capture_hwnd : s_active_hwnd;
    win32_window_t *w = user_get_window(target);
    if (w) {
        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);
        int rel_x = screen_x - sx;
        int rel_y = screen_y - sy;
        int cl_x = rel_x - w->client_x;
        int cl_y = rel_y - w->client_y;
        LPARAM lp = (uint32_t)(cl_y << 16) | (uint16_t)cl_x;
        user_post_message(w->hwnd, WM_MOUSEMOVE, 0, lp);
    }
}

void user_inject_key(int vkey, char ch, bool down) {
    if (s_active_hwnd) {
        H_HWND target = s_active_hwnd;
        /* If active window is a top-level frame containing a child Edit control, route keyboard input to the Edit control */
        for (int i = 1; i < MAX_WINDOWS; i++) {
            if (s_windows[i].in_use && s_windows[i].visible && s_windows[i].parent == s_active_hwnd &&
                strcasecmp(s_windows[i].class_name, "Edit") == 0) {
                target = s_windows[i].hwnd;
                break;
            }
        }
        UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
        user_post_message(target, msg, vkey, 0);
        if (down && ((ch >= 32 && ch <= 126) || ch == '\b' || ch == '\r' || ch == '\n' || ch == '\t')) {
            user_post_message(target, WM_CHAR, (uint8_t)ch, 0);
        }
    }
}

static void draw_canvas_text(uint8_t *canvas, int cw, int ch, int stride, int x, int y, const char *str, uint8_t color) {
    if (!canvas || !str) return;
    int len = (int)strlen(str);
    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c < 32 || c > 126) c = ' ';
        const uint8_t *glyph = GDI_FONT_8X8[c - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= ch) continue;
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                int px = x + i * 8 + gx;
                if (px < 0 || px >= cw) continue;
                if (row & (0x80 >> gx)) {
                    canvas[py * stride + px] = color;
                }
            }
        }
    }
}

/* Window Compositor: copies window client framebuffers onto canvas */
void user_composite_windows(uint8_t *dest_canvas, int canvas_w, int canvas_h, int canvas_stride) {
    if (!dest_canvas) return;

    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].in_use || !s_windows[i].visible || !s_windows[i].framebuffer) continue;
        win32_window_t *w = &s_windows[i];

        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);

        /* Blit window surface onto destination 8-bit grayscale canvas */
        for (int y = 0; y < w->height; y++) {
            int dy = sy + y;
            if (dy < 0 || dy >= canvas_h) continue;

            for (int x = 0; x < w->width; x++) {
                int dx = sx + x;
                if (dx < 0 || dx >= canvas_w) continue;

                uint32_t px = *(uint32_t *)(w->framebuffer + (y * w->fb_stride) + (x * 4));
                uint8_t r = (px >> 16) & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = px & 0xFF;
                uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);

                dest_canvas[(dy * canvas_stride) + dx] = gray;
            }
        }
    }

    /* Render active dropdown popup menu overlay if open */
    if (s_open_menu_hwnd != 0 && s_open_menu_idx >= 0) {
        win32_window_t *mw = user_get_window(s_open_menu_hwnd);
        if (mw && mw->visible && mw->menu != 0) {
            int m_slot = (int)(mw->menu - 0x5000);
            if (m_slot >= 1 && m_slot < MAX_MENUS && s_menus[m_slot].in_use) {
                win32_menu_t *m = &s_menus[m_slot];
                if (s_open_menu_idx < m->item_count && m->items[s_open_menu_idx].hsub != 0) {
                    int sub_slot = (int)(m->items[s_open_menu_idx].hsub - 0x5000);
                    if (sub_slot >= 1 && sub_slot < MAX_MENUS && s_menus[sub_slot].in_use) {
                        win32_menu_t *sub = &s_menus[sub_slot];
                        int sx = 0, sy = 0;
                        get_window_screen_pos(mw, &sx, &sy);
                        int drop_x = sx + 10;
                        for (int k = 0; k < s_open_menu_idx; k++) {
                            drop_x += (int)strlen(m->items[k].title) * 8 + 14;
                        }
                        int drop_y = sy + 43;
                        int drop_w = 180;
                        for (int idx = 0; idx < sub->item_count; idx++) {
                            int iw = (int)strlen(sub->items[idx].title) * 8 + 20;
                            if (iw > drop_w) drop_w = iw;
                        }
                        int drop_h = sub->item_count * 18 + 6;
                        for (int dy = drop_y; dy < drop_y + drop_h && dy < canvas_h; dy++) {
                            if (dy < 0) continue;
                            for (int dx = drop_x; dx < drop_x + drop_w && dx < canvas_w; dx++) {
                                if (dx < 0) continue;
                                uint8_t c = 0xE0;
                                if (dy == drop_y || dx == drop_x) c = 0xFF;
                                else if (dy == drop_y + drop_h - 1 || dx == drop_x + drop_w - 1) c = 0x00;
                                dest_canvas[dy * canvas_stride + dx] = c;
                            }
                        }
                        for (int idx = 0; idx < sub->item_count; idx++) {
                            int iy = drop_y + 3 + idx * 18;
                            if (sub->items[idx].flags & 0x0800) {
                                for (int dx = drop_x + 4; dx < drop_x + drop_w - 4 && dx < canvas_w; dx++) {
                                    if (iy + 8 < canvas_h) dest_canvas[(iy + 8) * canvas_stride + dx] = 0x80;
                                }
                            } else {
                                draw_canvas_text(dest_canvas, canvas_w, canvas_h, canvas_stride, drop_x + 8, iy + 4, sub->items[idx].title, 0x00);
                            }
                        }
                    }
                }
            }
        }
    }
}

static char wide_char_to_ascii(uint16_t ch) {
    if (ch > 0 && ch < 128) return (char)ch;
    switch (ch) {
        case 0x0131: return 'i'; /* ı */
        case 0x0130: return 'I'; /* İ */
        case 0x015F: return 's'; /* ş */
        case 0x015E: return 'S'; /* Ş */
        case 0x011F: return 'g'; /* ğ */
        case 0x011E: return 'G'; /* Ğ */
        case 0x00FC: case 0x00FB: case 0x00F9: case 0x00FA: return 'u';
        case 0x00DC: case 0x00DB: case 0x00D9: case 0x00DA: return 'U';
        case 0x00F6: case 0x00F4: case 0x00F2: case 0x00F3: return 'o';
        case 0x00D6: case 0x00D4: case 0x00D2: case 0x00D3: return 'O';
        case 0x00E7: return 'c'; /* ç */
        case 0x00C7: return 'C'; /* Ç */
        case 0x00E4: case 0x00E2: case 0x00E0: case 0x00E1: return 'a';
        case 0x00C4: case 0x00C2: case 0x00C0: case 0x00C1: return 'A';
        case 0x00EB: case 0x00EA: case 0x00E8: case 0x00E9: return 'e';
        case 0x00CB: case 0x00CA: case 0x00C8: case 0x00C9: return 'E';
        case 0x00A0: return ' ';
        default: return '?';
    }
}

static void wchar_to_ascii(const uint16_t *src, char *dst, size_t max_len) {
    if (!src || !dst || max_len == 0) return;
    size_t i = 0;
    while (src[i] != 0 && i < max_len - 1) {
        dst[i] = wide_char_to_ascii(src[i]);
        i++;
    }
    dst[i] = '\0';
}

static void ensure_edit_buf(win32_window_t *w, int min_cap) {
    if (!w) return;
    if (min_cap < 4096) min_cap = 4096;
    if (!w->edit_buf) {
        w->edit_buf = (char *)calloc(min_cap, 1);
        w->edit_cap = min_cap;
        w->edit_len = 0;
        if (w->title[0]) {
            strncpy(w->edit_buf, w->title, min_cap - 1);
            w->edit_len = (int)strlen(w->edit_buf);
            w->edit_caret = w->edit_len;
        }
    } else if (min_cap > w->edit_cap) {
        char *nb = (char *)realloc(w->edit_buf, min_cap);
        if (nb) {
            memset(nb + w->edit_cap, 0, min_cap - w->edit_cap);
            w->edit_buf = nb;
            w->edit_cap = min_cap;
        }
    }
}

static void sync_edit_guest_handle(win32_process_t *proc, win32_window_t *w) {
    if (!proc || !w) return;
    ensure_edit_buf(w, 4096);
    if (!w->edit_guest_handle) {
        uint32_t heap_args[3] = { 0x00010000, 0x00000008, 65536 };
        w->edit_guest_handle = k32_HeapAlloc(proc, heap_args);
    }
    if (w->edit_guest_handle) {
        /* Write both UTF-16 and ASCII depending on window creation, Notepad10 uses UTF-16 LocalLock(EM_GETHANDLE) */
        int max_chars = (w->edit_len < 32000) ? w->edit_len : 32000;
        for (int i = 0; i < max_chars; i++) {
            uint16_t ch = (uint8_t)w->edit_buf[i];
            guest_mem_write(&proc->mem, w->edit_guest_handle + i * 2, &ch, 2);
        }
        uint16_t zero = 0;
        guest_mem_write(&proc->mem, w->edit_guest_handle + max_chars * 2, &zero, 2);
    }
}

static void paint_builtin_control(win32_window_t *w, GUEST_PTR wndproc) {
    if (!w || !w->framebuffer || w->width <= 0 || w->height <= 0) return;
    uint8_t *fb = w->framebuffer;
    int bw = w->width;
    int bh = w->height;
    int stride = w->fb_stride;

    if (wndproc == BUILTIN_WNDPROC_EDIT) {
        ensure_edit_buf(w, 4096);
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFFFFFFF);
        if ((w->style & WS_BORDER) || (w->ex_style & 0x0200 /* WS_EX_CLIENTEDGE */)) {
            draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFF808080, 0xFFFFFFFF);
            if (bw > 2 && bh > 2) {
                draw_3d_rect_argb(fb, stride, bw, bh, 1, 1, bw - 2, bh - 2, 0xFF000000, 0xFFDFDFDF);
            }
        }
        int pad_x = 6, pad_y = 6;
        int cur_x = pad_x, cur_y = pad_y;
        int caret_px = pad_x, caret_py = pad_y;
        for (int i = 0; i <= w->edit_len; i++) {
            if (i == w->edit_caret) {
                caret_px = cur_x;
                caret_py = cur_y;
            }
            if (i == w->edit_len) break;
            char c = w->edit_buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                cur_x = pad_x;
                cur_y += 12;
                continue;
            }
            if (c == '\t') {
                cur_x += 32;
                continue;
            }
            if (cur_x + 8 > bw - 4) {
                if (!(w->style & 0x0040 /* ES_AUTOHSCROLL */)) {
                    cur_x = pad_x;
                    cur_y += 12;
                }
            }
            if (cur_y + 8 < bh) {
                char tmp[2] = { c, 0 };
                draw_text_argb(fb, stride, bw, bh, cur_x, cur_y, tmp, 0xFF000000);
            }
            cur_x += 8;
        }
        /* Draw blinking/solid vertical caret if active or child of active */
        if (caret_py + 10 < bh && caret_px < bw - 2) {
            fill_rect_argb(fb, stride, bw, bh, caret_px, caret_py - 1, 2, 10, 0xFF000000);
        }
    } else if (wndproc == BUILTIN_WNDPROC_BUTTON) {
        uint32_t btype = w->style & 0x0F;
        if (btype == 7 /* BS_GROUPBOX */) {
            fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFC0C0C0);
            draw_3d_rect_argb(fb, stride, bw, bh, 0, 4, bw, bh - 4, 0xFF808080, 0xFFFFFFFF);
            draw_3d_rect_argb(fb, stride, bw, bh, 1, 5, bw - 2, bh - 6, 0xFFFFFFFF, 0xFF808080);
            if (w->title[0]) {
                int tw = (int)strlen(w->title) * 8;
                fill_rect_argb(fb, stride, bw, bh, 8, 0, tw + 4, 10, 0xFFC0C0C0);
                draw_text_argb(fb, stride, bw, bh, 10, 1, w->title, 0xFF000000);
            }
        } else if (btype == 2 || btype == 3 || btype == 4 || btype == 9) {
            /* Checkbox / Radio button */
            fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFC0C0C0);
            int by = (bh - 12) / 2;
            fill_rect_argb(fb, stride, bw, bh, 2, by, 12, 12, 0xFFFFFFFF);
            draw_3d_rect_argb(fb, stride, bw, bh, 2, by, 12, 12, 0xFF808080, 0xFFFFFFFF);
            if (w->btn_check_state) {
                fill_rect_argb(fb, stride, bw, bh, 5, by + 3, 6, 6, 0xFF000000);
            }
            if (w->title[0]) {
                draw_text_argb(fb, stride, bw, bh, 18, (bh - 8) / 2, w->title, 0xFF000000);
            }
        } else {
            /* Push button */
            fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFC0C0C0);
            if (w->btn_pressed) {
                draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFF000000, 0xFFFFFFFF);
                draw_3d_rect_argb(fb, stride, bw, bh, 1, 1, bw - 2, bh - 2, 0xFF808080, 0xFFDFDFDF);
            } else {
                draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFFFFFFF, 0xFF000000);
                draw_3d_rect_argb(fb, stride, bw, bh, 1, 1, bw - 2, bh - 2, 0xFFDFDFDF, 0xFF808080);
            }
            if (w->title[0]) {
                int tw = (int)strlen(w->title) * 8;
                int tx = (bw - tw) / 2 + (w->btn_pressed ? 1 : 0);
                int ty = (bh - 8) / 2 + (w->btn_pressed ? 1 : 0);
                draw_text_argb(fb, stride, bw, bh, tx, ty, w->title, w->enabled ? 0xFF000000 : 0xFF808080);
            }
        }
    } else if (wndproc == BUILTIN_WNDPROC_STATIC) {
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFFFFFFF);
        if ((w->style & 0x1000 /* SS_SUNKEN */) || (w->style & WS_BORDER) || (w->ex_style & 0x0200 /* WS_EX_CLIENTEDGE */)) {
            draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFF808080, 0xFF000000);
        }
        if (w->title[0]) {
            int tx = 4;
            if ((w->style & 0x0F) == 1 /* SS_CENTER */) {
                tx = (bw - (int)strlen(w->title) * 8) / 2;
            } else if ((w->style & 0x0F) == 2 /* SS_RIGHT */) {
                tx = bw - (int)strlen(w->title) * 8 - 4;
            }
            draw_text_argb(fb, stride, bw, bh, tx, (bh - 8) / 2, w->title, 0xFF000000);
        }
    } else if (wndproc == BUILTIN_WNDPROC_STATUSBAR) {
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFC0C0C0);
        draw_3d_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFFFFFFF, 0xFF808080);
        if (w->sb_simple || w->sb_part_count <= 0) {
            const char *txt = w->sb_simple ? w->sb_simple_text : (w->sb_texts[0][0] ? w->sb_texts[0] : w->title);
            draw_3d_rect_argb(fb, stride, bw, bh, 2, 2, bw - 4, bh - 4, 0xFF808080, 0xFFFFFFFF);
            if (txt && txt[0]) {
                draw_text_argb(fb, stride, bw, bh, 6, (bh - 8) / 2, txt, 0xFF000000);
            }
        } else {
            int prev_right = 2;
            for (int i = 0; i < w->sb_part_count && i < 16; i++) {
                int right = w->sb_parts[i];
                if (right < 0 || right > bw - 2) right = bw - 2;
                int pw = right - prev_right - 2;
                if (pw > 4) {
                    draw_3d_rect_argb(fb, stride, bw, bh, prev_right, 2, pw, bh - 4, 0xFF808080, 0xFFFFFFFF);
                    if (w->sb_texts[i][0]) {
                        /* Strip leading tab if present (Win32 statusbar centers text after \t) */
                        const char *t = w->sb_texts[i];
                        while (*t == '\t') t++;
                        draw_text_argb(fb, stride, bw, bh, prev_right + 4, (bh - 8) / 2, t, 0xFF000000);
                    }
                }
                prev_right = right;
            }
        }
    }
}

static LRESULT builtin_wndproc_dispatch(win32_process_t *proc, win32_window_t *w, GUEST_PTR wndproc, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!w) return 0;

    if (wndproc == BUILTIN_WNDPROC_EDIT) {
        ensure_edit_buf(w, 4096);
        switch (msg) {
            case WM_CREATE:
            case WM_SIZE:
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            case WM_PAINT:
            case 0x0014 /* WM_ERASEBKGND */:
                paint_builtin_control(w, wndproc);
                s_windows_dirty = true;
                return 1;
            case 0x000C /* WM_SETTEXT */:
                if (lParam) {
                    uint16_t wbuf[1024] = {0};
                    guest_mem_read(&proc->mem, lParam, wbuf, sizeof(wbuf) - 2);
                    /* Detect whether string is ASCII or UTF-16 */
                    uint8_t raw[4] = {0};
                    guest_mem_read(&proc->mem, lParam, raw, 4);
                    if (raw[0] != 0 && raw[1] == 0) {
                        /* Likely UTF-16 */
                        int i = 0;
                        while (i < 1023 && wbuf[i]) {
                            w->edit_buf[i] = (char)(wbuf[i] & 0x7F);
                            i++;
                        }
                        w->edit_buf[i] = '\0';
                        w->edit_len = i;
                    } else {
                        const char *s = (const char *)guest_va_to_host(&proc->mem, lParam);
                        if (s) {
                            strncpy(w->edit_buf, s, w->edit_cap - 1);
                            w->edit_len = (int)strlen(w->edit_buf);
                        }
                    }
                } else {
                    w->edit_buf[0] = '\0';
                    w->edit_len = 0;
                }
                w->edit_caret = w->edit_len;
                strncpy(w->title, w->edit_buf, sizeof(w->title) - 1);
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 1;
            case 0x000D /* WM_GETTEXT */: {
                int max_c = (int)wParam;
                if (lParam && max_c > 0) {
                    int copy_len = (w->edit_len < max_c - 1) ? w->edit_len : (max_c - 1);
                    for (int i = 0; i < copy_len; i++) {
                        uint16_t ch = (uint8_t)w->edit_buf[i];
                        guest_mem_write(&proc->mem, lParam + i * 2, &ch, 2);
                    }
                    uint16_t z = 0;
                    guest_mem_write(&proc->mem, lParam + copy_len * 2, &z, 2);
                    return copy_len;
                }
                return 0;
            }
            case 0x000E /* WM_GETTEXTLENGTH */:
                return w->edit_len;
            case WM_CHAR: {
                char ch = (char)(wParam & 0xFF);
                if (ch == '\b') {
                    if (w->edit_caret > 0 && w->edit_len > 0) {
                        memmove(w->edit_buf + w->edit_caret - 1, w->edit_buf + w->edit_caret, w->edit_len - w->edit_caret + 1);
                        w->edit_caret--;
                        w->edit_len--;
                        w->edit_modified = true;
                    }
                } else if (ch == '\r' || ch == '\n') {
                    if (w->edit_len + 2 < w->edit_cap) {
                        memmove(w->edit_buf + w->edit_caret + 2, w->edit_buf + w->edit_caret, w->edit_len - w->edit_caret + 1);
                        w->edit_buf[w->edit_caret++] = '\r';
                        w->edit_buf[w->edit_caret++] = '\n';
                        w->edit_len += 2;
                        w->edit_modified = true;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    if (w->edit_len + 1 < w->edit_cap) {
                        memmove(w->edit_buf + w->edit_caret + 1, w->edit_buf + w->edit_caret, w->edit_len - w->edit_caret + 1);
                        w->edit_buf[w->edit_caret++] = ch;
                        w->edit_len++;
                        w->edit_modified = true;
                    }
                }
                strncpy(w->title, w->edit_buf, sizeof(w->title) - 1);
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                if (w->parent) {
                    uint32_t id = (uint32_t)w->menu;
                    user_post_message(w->parent, WM_COMMAND, (0x0300 /* EN_CHANGE */ << 16) | (id & 0xFFFF), (LPARAM)w->hwnd);
                }
                return 0;
            }
            case WM_KEYDOWN: {
                if (wParam == 0x25 /* VK_LEFT */ && w->edit_caret > 0) w->edit_caret--;
                else if (wParam == 0x27 /* VK_RIGHT */ && w->edit_caret < w->edit_len) w->edit_caret++;
                else if (wParam == 0x24 /* VK_HOME */) w->edit_caret = 0;
                else if (wParam == 0x23 /* VK_END */) w->edit_caret = w->edit_len;
                else if (wParam == 0x2E /* VK_DELETE */ && w->edit_caret < w->edit_len) {
                    memmove(w->edit_buf + w->edit_caret, w->edit_buf + w->edit_caret + 1, w->edit_len - w->edit_caret);
                    w->edit_len--;
                    w->edit_modified = true;
                }
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            }
            case 0x00B0 /* EM_GETSEL */:
                if (wParam) x86_mem_write32(&proc->cpu, wParam, w->edit_caret);
                if (lParam) x86_mem_write32(&proc->cpu, lParam, w->edit_caret);
                return (w->edit_caret << 16) | (w->edit_caret & 0xFFFF);
            case 0x00B1 /* EM_SETSEL */:
                w->edit_sel_start = (int)wParam;
                w->edit_sel_end = (int)lParam;
                if (w->edit_sel_end < 0 || w->edit_sel_end > w->edit_len) w->edit_caret = w->edit_len;
                else w->edit_caret = w->edit_sel_end;
                paint_builtin_control(w, wndproc);
                return 1;
            case 0x00B8 /* EM_GETMODIFY */:
                return w->edit_modified ? 1 : 0;
            case 0x00B9 /* EM_SETMODIFY */:
                w->edit_modified = (wParam != 0);
                return 0;
            case 0x00BA /* EM_GETLINECOUNT */: {
                int lines = 1;
                for (int i = 0; i < w->edit_len; i++) if (w->edit_buf[i] == '\n') lines++;
                return lines;
            }
            case 0x00BB /* EM_LINEINDEX */: {
                int target_line = (int)wParam;
                if (target_line < 0) {
                    /* Current caret line */
                    int last_nl = 0;
                    for (int i = 0; i < w->edit_caret && i < w->edit_len; i++) {
                        if (w->edit_buf[i] == '\n') last_nl = i + 1;
                    }
                    return last_nl;
                }
                int cur_line = 0;
                for (int i = 0; i < w->edit_len; i++) {
                    if (cur_line == target_line) return i;
                    if (w->edit_buf[i] == '\n') cur_line++;
                }
                return (cur_line == target_line) ? w->edit_len : (LRESULT)-1;
            }
            case 0x00C1 /* EM_LINELENGTH */:
                return w->edit_len;
            case 0x00C2 /* EM_REPLACESEL */: {
                if (lParam) {
                    uint16_t wbuf[512] = {0};
                    guest_mem_read(&proc->mem, lParam, wbuf, sizeof(wbuf) - 2);
                    char ins[512] = {0};
                    wchar_to_ascii(wbuf, ins, sizeof(ins));
                    int ilen = (int)strlen(ins);
                    if (w->edit_len + ilen < w->edit_cap) {
                        memmove(w->edit_buf + w->edit_caret + ilen, w->edit_buf + w->edit_caret, w->edit_len - w->edit_caret + 1);
                        memcpy(w->edit_buf + w->edit_caret, ins, ilen);
                        w->edit_caret += ilen;
                        w->edit_len += ilen;
                        w->edit_modified = true;
                        paint_builtin_control(w, wndproc);
                        w->needs_redraw = true;
                        s_windows_dirty = true;
                    }
                }
                return 1;
            }
            case 0x00C9 /* EM_LINEFROMCHAR */: {
                int pos = (int)wParam;
                if (pos < 0 || pos > w->edit_len) pos = w->edit_caret;
                int line = 0;
                for (int i = 0; i < pos && i < w->edit_len; i++) {
                    if (w->edit_buf[i] == '\n') line++;
                }
                return line;
            }
            case 0x00BD /* EM_GETHANDLE */:
                sync_edit_guest_handle(proc, w);
                return w->edit_guest_handle;
            default:
                return 0;
        }
    } else if (wndproc == BUILTIN_WNDPROC_BUTTON) {
        switch (msg) {
            case WM_CREATE:
            case WM_PAINT:
            case 0x0014 /* WM_ERASEBKGND */:
                paint_builtin_control(w, wndproc);
                s_windows_dirty = true;
                return 1;
            case WM_LBUTTONDOWN:
                w->btn_pressed = true;
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            case WM_LBUTTONUP:
                w->btn_pressed = false;
                if ((w->style & 0x0F) == 3 /* BS_AUTOCHECKBOX */) {
                    w->btn_check_state = !w->btn_check_state;
                } else if ((w->style & 0x0F) == 9 /* BS_AUTORADIOBUTTON */) {
                    w->btn_check_state = 1;
                }
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                if (w->parent) {
                    uint32_t ctrl_id = (uint32_t)w->menu;
                    user_post_message(w->parent, WM_COMMAND, ctrl_id & 0xFFFF, (LPARAM)w->hwnd);
                }
                return 0;
            case 0x00F0 /* BM_GETCHECK */:
                return w->btn_check_state;
            case 0x00F1 /* BM_SETCHECK */:
                w->btn_check_state = (int)wParam;
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            case 0x00F2 /* BM_GETSTATE */:
                return (w->btn_pressed ? 0x0004 : 0) | w->btn_check_state;
            case 0x00F3 /* BM_SETSTATE */:
                w->btn_pressed = (wParam != 0);
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            default:
                return 0;
        }
    } else if (wndproc == BUILTIN_WNDPROC_STATIC) {
        switch (msg) {
            case WM_CREATE:
            case WM_PAINT:
            case 0x0014 /* WM_ERASEBKGND */:
                paint_builtin_control(w, wndproc);
                s_windows_dirty = true;
                return 1;
            default:
                return 0;
        }
    } else if (wndproc == BUILTIN_WNDPROC_STATUSBAR) {
        switch (msg) {
            case WM_CREATE:
            case WM_PAINT:
            case 0x0014 /* WM_ERASEBKGND */:
                paint_builtin_control(w, wndproc);
                s_windows_dirty = true;
                return 1;
            case WM_SIZE: {
                win32_window_t *pw = user_get_window(w->parent);
                if (pw) {
                    int nh = 22;
                    int nw = pw->client_w;
                    int nx = 0;
                    int ny = pw->client_h - nh;
                    if (ny < 0) ny = 0;
                    if (nw > 0 && (w->width != nw || w->height != nh || w->x != nx || w->y != ny)) {
                        w->x = nx;
                        w->y = ny;
                        w->width = nw;
                        w->height = nh;
                        w->fb_stride = nw * 4;
                        if (w->framebuffer) free(w->framebuffer);
                        w->framebuffer = allocate_window_framebuffer(nw, nh, 0xFFC0C0C0);
                        update_window_client_area(w);
                    }
                }
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 0;
            }
            case 0x0404 /* SB_SETPARTS */: {
                int count = (int)wParam;
                if (count > 16) count = 16;
                w->sb_part_count = count;
                if (lParam && count > 0) {
                    guest_mem_read(&proc->mem, lParam, w->sb_parts, count * sizeof(int));
                }
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 1;
            }
            case 0x0401 /* SB_SETTEXTA */:
            case 0x040B /* SB_SETTEXTW */: {
                int part = (int)(wParam & 0xFF);
                char buf[128] = {0};
                if (lParam) {
                    if (msg == 0x040B) {
                        uint16_t wbuf[128] = {0};
                        guest_mem_read(&proc->mem, lParam, wbuf, sizeof(wbuf) - 2);
                        wchar_to_ascii(wbuf, buf, sizeof(buf));
                    } else {
                        const char *s = (const char *)guest_va_to_host(&proc->mem, lParam);
                        if (s) strncpy(buf, s, sizeof(buf) - 1);
                    }
                }
                if (part == 255) {
                    strncpy(w->sb_simple_text, buf, sizeof(w->sb_simple_text) - 1);
                } else if (part >= 0 && part < 16) {
                    strncpy(w->sb_texts[part], buf, sizeof(w->sb_texts[part]) - 1);
                    if (part >= w->sb_part_count) w->sb_part_count = part + 1;
                }
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 1;
            }
            case 0x0409 /* SB_SIMPLE */:
                w->sb_simple = (wParam != 0);
                paint_builtin_control(w, wndproc);
                w->needs_redraw = true;
                s_windows_dirty = true;
                return 1;
            default:
                return 0;
        }
    }
    return 0;
}

static uint32_t u32_LoadStringW(win32_process_t *proc, uint32_t *args) {
    uint32_t uID = args[1];
    uint32_t buf_va = args[2];
    int max_chars = (int)args[3];

    if (!buf_va || max_chars <= 0) return 0;

    uint16_t temp_buf[256] = {0};
    int len = pe_load_resource_string(proc, uID, temp_buf, sizeof(temp_buf)/2);
    if (len > 0) {
        guest_mem_write(&proc->mem, buf_va, temp_buf, (len + 1) * 2);
        char ascii[128] = {0};
        wchar_to_ascii(temp_buf, ascii, sizeof(ascii));
        LOGI("LoadStringW: ID %u -> \"%s\" (%d chars)", uID, ascii, len);
        return (uint32_t)len;
    }

    LOGW("LoadStringW: ID %u not found in resources", uID);
    return 0;
}

static uint32_t u32_RegisterClassW(win32_process_t *proc, uint32_t *args) {
    uint32_t wc_va = args[0];
    if (!wc_va || s_class_count >= MAX_CLASSES) return 0;

    uint32_t wndproc = 0, name_va = 0, icon = 0, cursor = 0, brush = 0, menu_va = 0;
    guest_mem_read(&proc->mem, wc_va + 4, &wndproc, 4);
    guest_mem_read(&proc->mem, wc_va + 20, &icon, 4);
    guest_mem_read(&proc->mem, wc_va + 24, &cursor, 4);
    guest_mem_read(&proc->mem, wc_va + 28, &brush, 4);
    guest_mem_read(&proc->mem, wc_va + 32, &menu_va, 4);
    guest_mem_read(&proc->mem, wc_va + 36, &name_va, 4);

    uint16_t wide_name[64] = {0};
    guest_mem_read(&proc->mem, name_va, wide_name, sizeof(wide_name));
    char ascii_name[64] = {0};
    wchar_to_ascii(wide_name, ascii_name, sizeof(ascii_name));

    strncpy(s_classes[s_class_count].class_name, ascii_name, sizeof(s_classes[0].class_name) - 1);
    s_classes[s_class_count].wndproc = wndproc;
    s_classes[s_class_count].icon = icon;
    s_classes[s_class_count].cursor = cursor;
    s_classes[s_class_count].background_brush = brush;
    s_classes[s_class_count].menu_id = menu_va;

    LOGI("RegisterClassW: %s (WndProc=0x%08X, menu=0x%X)", ascii_name, wndproc, menu_va);
    s_class_count++;
    return 0x0001;
}

static uint32_t u32_CreateWindowExW(win32_process_t *proc, uint32_t *args) {
    uint32_t ex_style = args[0];
    uint32_t class_va = args[1];
    uint32_t title_va = args[2];
    uint32_t style = args[3];
    int x = (int)args[4];
    int y = (int)args[5];
    int w = (int)args[6];
    int h = (int)args[7];
    H_HWND parent = (H_HWND)args[8];
    H_HMENU menu = (H_HMENU)args[9];

    char class_name[64] = {0};
    char title[128] = {0};

    if (class_va) {
        uint16_t wbuf[64] = {0};
        guest_mem_read(&proc->mem, class_va, wbuf, sizeof(wbuf));
        wchar_to_ascii(wbuf, class_name, sizeof(class_name));
    }
    if (title_va) {
        uint16_t wbuf[128] = {0};
        guest_mem_read(&proc->mem, title_va, wbuf, sizeof(wbuf));
        wchar_to_ascii(wbuf, title, sizeof(title));
    }

    /* Standard Win32 default coordinates */
    if (x == (int)0x80000000 || x < 0) x = (style & WS_CHILD) ? 0 : ((DEFAULT_SCREEN_W > 640) ? (DEFAULT_SCREEN_W - 640) / 2 : 40);
    if (y == (int)0x80000000 || y < 0) y = (style & WS_CHILD) ? 0 : ((DEFAULT_SCREEN_H > 480) ? (DEFAULT_SCREEN_H - 480) / 2 : 40);
    if (w <= 0 || w == (int)0x80000000) w = (style & WS_CHILD) ? 100 : 640;
    if (h <= 0 || h == (int)0x80000000) h = (style & WS_CHILD) ? 20 : 480;

    int slot = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return 0;

    H_HWND hwnd = (H_HWND)(0x4000 + slot);
    win32_window_t *win = &s_windows[slot];
    memset(win, 0, sizeof(win32_window_t));
    win->in_use = true;
    win->hwnd = hwnd;
    win->parent = parent;
    win->style = style;
    win->ex_style = ex_style;
    win->menu = menu;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->visible = (style & WS_VISIBLE) != 0;
    win->enabled = true;

    strncpy(win->class_name, class_name, sizeof(win->class_name) - 1);
    strncpy(win->title, title, sizeof(win->title) - 1);

    for (int i = 0; i < s_class_count; i++) {
        if (strcasecmp(s_classes[i].class_name, win->class_name) == 0) {
            win->wndproc = s_classes[i].wndproc;
            if (!(style & WS_CHILD) && win->menu == 0 && s_classes[i].menu_id != 0) {
                win->menu = (H_HMENU)load_menu_internal(proc, s_classes[i].menu_id, true);
            }
            break;
        }
    }

    win->hdc = gdi_create_window_dc(hwnd);
    win->fb_stride = w * 4;
    uint32_t bg_col = (strcasecmp(win->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
    win->framebuffer = allocate_window_framebuffer(w, h, bg_col);

    gdi_dc_t *dc = gdi_get_dc(win->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        if (bm_obj) {
            bm_obj->bitmap.bpp = 32;
            dc->selected_bitmap = (H_HBITMAP)hbm;
        }
    }
    update_window_client_area(win);
    paint_window_decorations(win);

    s_active_hwnd = (style & WS_CHILD) ? (s_active_hwnd ? s_active_hwnd : parent) : hwnd;
    LOGI("CreateWindowExW: '%s' (%s) -> HWND 0x%08X [%d,%d %dx%d]",
         win->title, win->class_name, hwnd, x, y, w, h);

    user_send_message(proc, hwnd, WM_CREATE, 0, 0, NULL);
    user_send_message(proc, hwnd, WM_SIZE, 0, ((uint32_t)win->client_h << 16) | (win->client_w & 0xFFFF), NULL);
    return (uint32_t)hwnd;
}

static uint32_t u32_GetSystemMetrics(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t idx = args[0];
    switch (idx) {
        case 0: return DEFAULT_SCREEN_W; /* SM_CXSCREEN */
        case 1: return DEFAULT_SCREEN_H; /* SM_CYSCREEN */
        case 4: return 24;               /* SM_CYCAPTION */
        case 5: return 2;                /* SM_CXBORDER */
        case 6: return 2;                /* SM_CYBORDER */
        case 11: return 0;               /* SM_SWAPBUTTON */
        default: return 0;
    }
}

static uint32_t u32_MessageBoxW(win32_process_t *proc, uint32_t *args) {
    uint32_t text_va = args[1];
    uint32_t cap_va  = args[2];
    char text[256] = {0};
    char cap[128] = {0};

    if (text_va) {
        uint16_t wbuf[256] = {0};
        guest_mem_read(&proc->mem, text_va, wbuf, sizeof(wbuf));
        wchar_to_ascii(wbuf, text, sizeof(text));
    }
    if (cap_va) {
        uint16_t wbuf[128] = {0};
        guest_mem_read(&proc->mem, cap_va, wbuf, sizeof(wbuf));
        wchar_to_ascii(wbuf, cap, sizeof(cap));
    }

    LOGI(">>> MessageBoxW: [%s] %s <<<", cap, text);
    return 1; /* IDOK */
}

static uint32_t u32_dummy_handle(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    static uint32_t s_dummy_h = 0x5000;
    return ++s_dummy_h;
}

static uint32_t u32_GetMenuItemRect(win32_process_t *proc, uint32_t *args) {
    uint32_t rect_va = args[3];
    if (rect_va) {
        WIN_RECT rc = { 0, 0, 30, 20 };
        guest_mem_write(&proc->mem, rect_va, &rc, sizeof(rc));
    }
    return 1; /* TRUE */
}

static uint32_t u32_MoveWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    int w = (int)args[3];
    int h = (int)args[4];
    BOOL bRepaint = (BOOL)args[5];

    win32_window_t *win = user_get_window(hwnd);
    if (win) {
        if ((win->style & WS_CHILD) || x >= 0) win->x = x;
        if ((win->style & WS_CHILD) || y >= 0) win->y = y;
        bool resized = false;
        if (w > 0 && h > 0 && (w != win->width || h != win->height)) {
            win->width = w;
            win->height = h;
            win->fb_stride = w * 4;
            if (win->framebuffer) free(win->framebuffer);
            uint32_t bg_col = (strcasecmp(win->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
            win->framebuffer = allocate_window_framebuffer(w, h, bg_col);
            update_window_client_area(win);
            if (win->wndproc >= 0xFE010000U && win->wndproc <= 0xFE0100FFU) {
                paint_builtin_control(win, win->wndproc);
            } else {
                paint_window_decorations(win);
            }
            resized = true;
        }
        if (resized || bRepaint) {
            win->needs_redraw = true;
            s_windows_dirty = true;
            user_post_message(hwnd, WM_PAINT, 0, 0);
        }
        LOGI("MoveWindow: HWND 0x%08X to [%d,%d %dx%d]", hwnd, win->x, win->y, win->width, win->height);
        return 1;
    }
    return 0;
}

static uint32_t u32_GetDesktopWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00010000; /* Desktop HWND */
}

static uint32_t u32_CheckMenuItem(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* MF_UNCHECKED */
}

static uint32_t u32_SetMenu(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    H_HMENU hmenu = (H_HMENU)args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        w->menu = hmenu;
        update_window_client_area(w);
        paint_window_decorations(w);
        w->needs_redraw = true;
    }
    return 1;
}

static uint32_t u32_GetDlgItemInt(win32_process_t *proc, uint32_t *args) {
    uint32_t lpTrans = args[2];
    if (lpTrans) {
        uint32_t ok = 1;
        guest_mem_write(&proc->mem, lpTrans, &ok, sizeof(ok));
    }
    return 10;
}

static uint32_t u32_ReleaseCapture(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    s_capture_hwnd = 0;
    return 1;
}

static uint32_t u32_SetCapture(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND prev = s_capture_hwnd;
    s_capture_hwnd = (H_HWND)args[0];
    return (uint32_t)prev;
}

static uint32_t u32_MapWindowPoints(win32_process_t *proc, uint32_t *args) {
    H_HWND hWndFrom = (H_HWND)args[0];
    H_HWND hWndTo   = (H_HWND)args[1];
    uint32_t lpPoints = args[2];
    uint32_t cPoints  = args[3];

    if (!lpPoints || cPoints == 0) return 0;

    int from_sx = 0, from_sy = 0;
    if (hWndFrom) {
        win32_window_t *wFrom = user_get_window(hWndFrom);
        if (wFrom) {
            get_window_screen_pos(wFrom, &from_sx, &from_sy);
            from_sx += wFrom->client_x;
            from_sy += wFrom->client_y;
        }
    }

    int to_sx = 0, to_sy = 0;
    if (hWndTo) {
        win32_window_t *wTo = user_get_window(hWndTo);
        if (wTo) {
            get_window_screen_pos(wTo, &to_sx, &to_sy);
            to_sx += wTo->client_x;
            to_sy += wTo->client_y;
        }
    }

    int dx = from_sx - to_sx;
    int dy = from_sy - to_sy;

    for (uint32_t i = 0; i < cPoints; i++) {
        WIN_POINT pt;
        if (guest_mem_read(&proc->mem, lpPoints + i * sizeof(WIN_POINT), &pt, sizeof(pt))) {
            pt.x += dx;
            pt.y += dy;
            guest_mem_write(&proc->mem, lpPoints + i * sizeof(WIN_POINT), &pt, sizeof(pt));
        }
    }

    return (uint32_t)((dy << 16) | (dx & 0xFFFF));
}

static uint32_t u32_PtInRect(win32_process_t *proc, uint32_t *args) {
    uint32_t lprc_va = args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    WIN_RECT rc = {0};
    if (!guest_mem_read(&proc->mem, lprc_va, &rc, sizeof(rc))) return 0;
    bool inside = (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom);
    LOGI("PtInRect: lprc=0x%08X rc=[%d,%d,%d,%d] pt=(%d,%d) -> %s",
         lprc_va, rc.left, rc.top, rc.right, rc.bottom, x, y, inside ? "TRUE" : "FALSE");
    return inside ? 1 : 0;
}

static uint32_t u32_SetRect(win32_process_t *proc, uint32_t *args) {
    uint32_t lprc_va = args[0];
    WIN_RECT rc = {
        .left = (int32_t)args[1],
        .top = (int32_t)args[2],
        .right = (int32_t)args[3],
        .bottom = (int32_t)args[4]
    };
    guest_mem_write(&proc->mem, lprc_va, &rc, sizeof(rc));
    return 1;
}

static uint32_t u32_InvalidateRect(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    if (hwnd) {
        win32_window_t *w = user_get_window(hwnd);
        if (w) w->needs_redraw = true;
        s_windows_dirty = true;
        user_post_message(hwnd, WM_PAINT, 0, 0);
    }
    return 1;
}

static uint32_t u32_WinHelpW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_SetDlgItemInt(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_EndDialog(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_GetDlgItem(win32_process_t *proc, uint32_t *args);
static uint32_t u32_GetWindowTextW(win32_process_t *proc, uint32_t *args);

static uint32_t u32_SetDlgItemTextW(win32_process_t *proc, uint32_t *args) {
    uint32_t gdi_args[2] = { args[0], args[1] };
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, gdi_args);
    if (hctrl) {
        win32_window_t *cw = user_get_window(hctrl);
        if (cw && args[2]) {
            uint16_t wbuf[128] = {0};
            guest_mem_read(&proc->mem, args[2], wbuf, sizeof(wbuf) - 2);
            wchar_to_ascii(wbuf, cw->title, sizeof(cw->title));
            if (cw->wndproc >= 0xFE010000U && cw->wndproc <= 0xFE0100FFU) {
                paint_builtin_control(cw, cw->wndproc);
            } else {
                paint_window_decorations(cw);
            }
            cw->needs_redraw = true;
            s_windows_dirty = true;
        }
    }
    return 1;
}

static uint32_t handle_send_message(win32_process_t *proc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool unicode) {
    win32_window_t *w_check = user_get_window(hwnd);
    if (w_check && w_check->wndproc >= 0xFE010000U && w_check->wndproc <= 0xFE0100FFU) {
        return (uint32_t)builtin_wndproc_dispatch(proc, w_check, w_check->wndproc, msg, wParam, lParam);
    }
    if (msg == 0x0220 /* WM_MDICREATE */) {
        uint32_t szClass_va = 0;
        uint32_t szTitle_va = 0;
        int32_t x = 20, y = 20, cx = 360, cy = 240;
        uint32_t style = 0;
        uint32_t user_lParam = 0;

        guest_mem_read(&proc->mem, lParam, &szClass_va, 4);
        guest_mem_read(&proc->mem, lParam + 4, &szTitle_va, 4);
        guest_mem_read(&proc->mem, lParam + 12, &x, 4);
        guest_mem_read(&proc->mem, lParam + 16, &y, 4);
        guest_mem_read(&proc->mem, lParam + 20, &cx, 4);
        guest_mem_read(&proc->mem, lParam + 24, &cy, 4);
        guest_mem_read(&proc->mem, lParam + 28, &style, 4);
        guest_mem_read(&proc->mem, lParam + 32, &user_lParam, 4);

        char class_name[64] = {0};
        char title[128] = {0};

        if (szClass_va) {
            if (unicode) {
                uint16_t wbuf[64] = {0};
                guest_mem_read(&proc->mem, szClass_va, wbuf, sizeof(wbuf));
                wchar_to_ascii(wbuf, class_name, sizeof(class_name));
            } else {
                const char *s = (const char *)guest_va_to_host(&proc->mem, szClass_va);
                if (s) strncpy(class_name, s, sizeof(class_name) - 1);
            }
        }
        if (szTitle_va) {
            if (unicode) {
                uint16_t wbuf[128] = {0};
                guest_mem_read(&proc->mem, szTitle_va, wbuf, sizeof(wbuf));
                wchar_to_ascii(wbuf, title, sizeof(title));
            } else {
                const char *s = (const char *)guest_va_to_host(&proc->mem, szTitle_va);
                if (s) strncpy(title, s, sizeof(title) - 1);
            }
        }

        if (x == (int)0x80000000 || x < 0) x = 15;
        if (y == (int)0x80000000 || y < 0) y = 15;
        if (cx <= 0 || cx == (int)0x80000000) cx = 460;
        if (cy <= 0 || cy == (int)0x80000000) cy = 300;

        int slot = -1;
        for (int i = 1; i < MAX_WINDOWS; i++) {
            if (!s_windows[i].in_use) { slot = i; break; }
        }
        if (slot == -1) return 0;

        H_HWND child_hwnd = (H_HWND)(0x4000 + slot);
        win32_window_t *win = &s_windows[slot];
        memset(win, 0, sizeof(win32_window_t));
        win->in_use = true;
        win->hwnd = child_hwnd;
        win->parent = hwnd;
        win->style = style | WS_CHILD | WS_VISIBLE;
        win->x = x;
        win->y = y;
        win->width = cx;
        win->height = cy;
        win->visible = true;
        win->enabled = true;

        strncpy(win->class_name, class_name, sizeof(win->class_name) - 1);
        strncpy(win->title, title, sizeof(win->title) - 1);

        for (int i = 0; i < s_class_count; i++) {
            if (strcasecmp(s_classes[i].class_name, win->class_name) == 0) {
                win->wndproc = s_classes[i].wndproc;
                break;
            }
        }

        win->hdc = gdi_create_window_dc(child_hwnd);
        win->fb_stride = cx * 4;
        win->framebuffer = allocate_window_framebuffer(cx, cy, 0xFFC0C0C0);
        paint_window_decorations(win);

        gdi_dc_t *dc = gdi_get_dc(win->hdc);
        if (dc) {
            H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
            gdi_object_t *bm_obj = gdi_get_obj(hbm);
            if (bm_obj) {
                bm_obj->bitmap.width = cx;
                bm_obj->bitmap.height = cy;
                bm_obj->bitmap.bpp = 32;
                bm_obj->bitmap.stride = win->fb_stride;
                bm_obj->bitmap.bits = win->framebuffer;
                bm_obj->bitmap.is_window_fb = true;
                dc->selected_bitmap = (H_HBITMAP)hbm;
            }
        }

        s_active_hwnd = child_hwnd;
        LOGI("WM_MDICREATE: child '%s' (%s) -> HWND 0x%08X [%d,%d %dx%d]",
             win->title, win->class_name, child_hwnd, x, y, cx, cy);

        user_send_message(proc, child_hwnd, WM_CREATE, 0, lParam, NULL);
        return (uint32_t)child_hwnd;
    }

    if (msg == 0x000C /* WM_SETTEXT */) {
        win32_window_t *w = user_get_window(hwnd);
        if (w && lParam) {
            if (unicode) {
                uint16_t wbuf[128] = {0};
                guest_mem_read(&proc->mem, lParam, wbuf, sizeof(wbuf));
                wchar_to_ascii(wbuf, w->title, sizeof(w->title));
            } else {
                const char *s = (const char *)guest_va_to_host(&proc->mem, lParam);
                if (s) strncpy(w->title, s, sizeof(w->title) - 1);
            }
            w->needs_redraw = true;
            return 1;
        }
    }

    /* General window message dispatch */
    win32_window_t *target = user_get_window(hwnd);
    if (target && target->wndproc) {
        LRESULT res = 0;
        user_send_message(proc, hwnd, msg, wParam, lParam, &res);
        return (uint32_t)res;
    }

    return 0;
}

static uint32_t u32_SendMessageW(win32_process_t *proc, uint32_t *args) {
    return handle_send_message(proc, (H_HWND)args[0], args[1], args[2], args[3], true);
}

static uint32_t u32_SendMessageA(win32_process_t *proc, uint32_t *args) {
    return handle_send_message(proc, (H_HWND)args[0], args[1], args[2], args[3], false);
}

static uint32_t u32_GetDlgItem(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwndDlg = (H_HWND)args[0];
    uint32_t ctrl_id = args[1];
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (s_windows[i].in_use && (hwndDlg == 0 || s_windows[i].parent == hwndDlg)) {
            if (((uint32_t)s_windows[i].menu & 0xFFFF) == (ctrl_id & 0xFFFF)) {
                return (uint32_t)s_windows[i].hwnd;
            }
        }
    }
    return 0;
}

static uint32_t u32_GetDlgItemTextW(win32_process_t *proc, uint32_t *args) {
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, args);
    uint32_t buf_va = args[2];
    int max_len = (int)args[3];
    if (hctrl) {
        uint32_t gt_args[3] = { hctrl, buf_va, (uint32_t)max_len };
        return u32_GetWindowTextW(proc, gt_args);
    }
    if (buf_va && max_len > 0) {
        uint16_t zero = 0;
        guest_mem_write(&proc->mem, buf_va, &zero, sizeof(zero));
    }
    return 0;
}

static const uint8_t *skip_dlg_sz_or_ord(const uint8_t *p, const uint8_t *end, char *out_str, size_t max_out, uint16_t *out_ord) {
    if (out_str && max_out > 0) out_str[0] = '\0';
    if (out_ord) *out_ord = 0;
    if (p + 2 > end) return end;
    uint16_t first = *(const uint16_t *)p;
    if (first == 0x0000) {
        return p + 2;
    }
    if (first == 0xFFFF) {
        if (p + 4 <= end && out_ord) {
            *out_ord = *(const uint16_t *)(p + 2);
        }
        return (p + 4 <= end) ? (p + 4) : end;
    }
    size_t i = 0;
    while (p + 2 <= end) {
        uint16_t ch = *(const uint16_t *)p;
        p += 2;
        if (ch == 0) break;
        if (out_str && i + 1 < max_out && ch != '&') {
            out_str[i++] = wide_char_to_ascii(ch);
        }
    }
    if (out_str && max_out > 0) out_str[i] = '\0';
    return p;
}

static uint32_t load_menu_internal(win32_process_t *proc, uint32_t menu_id_or_name, bool is_unicode);

static uint32_t create_dialog_from_template(win32_process_t *proc, uint32_t tmpl_id_or_name, H_HWND hwndParent, GUEST_PTR dlgProc, LPARAM dwInitParam, bool is_unicode, bool is_direct_ptr) {
    const uint8_t *data = NULL;
    uint32_t data_sz = 4096;
    if (is_direct_ptr) {
        data = guest_va_to_host(&proc->mem, tmpl_id_or_name);
    } else {
        uint32_t rva = pe_find_resource_ex(proc, 5 /* RT_DIALOG */, tmpl_id_or_name, is_unicode, &data_sz);
        if (rva) data = guest_va_to_host(&proc->mem, rva);
    }

    int slot = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return 0;

    H_HWND hwndDlg = (H_HWND)(0x4000 + slot);
    win32_window_t *dlg = &s_windows[slot];
    memset(dlg, 0, sizeof(win32_window_t));
    dlg->in_use = true;
    dlg->hwnd = hwndDlg;
    dlg->parent = hwndParent;
    dlg->wndproc = dlgProc;
    dlg->enabled = true;
    dlg->visible = true;
    strcpy(dlg->class_name, "#32770");

    if (!data || data_sz < 18) {
        dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
        dlg->x = 120; dlg->y = 80; dlg->width = 360; dlg->height = 240;
        dlg->hdc = gdi_create_window_dc(hwndDlg);
        dlg->fb_stride = dlg->width * 4;
        dlg->framebuffer = allocate_window_framebuffer(dlg->width, dlg->height, 0xFFC0C0C0);
        update_window_client_area(dlg);
        paint_window_decorations(dlg);
        if (dlgProc) call_wndproc(proc, dlgProc, hwndDlg, 0x0110 /* WM_INITDIALOG */, 0, dwInitParam);
        return (uint32_t)hwndDlg;
    }

    const uint8_t *end = data + data_sz;
    const uint8_t *p = data;
    bool is_ex = (*(const uint16_t *)(p + 0) == 1 && *(const uint16_t *)(p + 2) == 0xFFFF);

    uint32_t style = 0, ex_style = 0;
    uint16_t cdit = 0;
    int16_t dx = 0, dy = 0, dcx = 200, dcy = 120;

    if (is_ex && p + 26 <= end) {
        ex_style = *(const uint32_t *)(p + 8);
        style    = *(const uint32_t *)(p + 12);
        cdit     = *(const uint16_t *)(p + 16);
        dx       = *(const int16_t *)(p + 18);
        dy       = *(const int16_t *)(p + 20);
        dcx      = *(const int16_t *)(p + 22);
        dcy      = *(const int16_t *)(p + 24);
        p += 26;
    } else {
        style    = *(const uint32_t *)(p + 0);
        ex_style = *(const uint32_t *)(p + 4);
        cdit     = *(const uint16_t *)(p + 8);
        dx       = *(const int16_t *)(p + 10);
        dy       = *(const int16_t *)(p + 12);
        dcx      = *(const int16_t *)(p + 14);
        dcy      = *(const int16_t *)(p + 16);
        p += 18;
    }

    /* Menu, Class, Title */
    char dlg_menu_name[64] = {0};
    uint16_t dlg_menu_ord = 0;
    p = skip_dlg_sz_or_ord(p, end, dlg_menu_name, sizeof(dlg_menu_name), &dlg_menu_ord);
    char dlg_class_name[64] = {0};
    uint16_t dlg_class_ord = 0;
    p = skip_dlg_sz_or_ord(p, end, dlg_class_name, sizeof(dlg_class_name), &dlg_class_ord);
    p = skip_dlg_sz_or_ord(p, end, dlg->title, sizeof(dlg->title), NULL);

    if (dlg_class_name[0] != '\0') {
        strncpy(dlg->class_name, dlg_class_name, sizeof(dlg->class_name) - 1);
        for (int k = 0; k < s_class_count; k++) {
            if (strcasecmp(s_classes[k].class_name, dlg->class_name) == 0) {
                if (!dlg->wndproc) dlg->wndproc = s_classes[k].wndproc;
                if (!dlg->menu && s_classes[k].menu_id != 0) {
                    dlg->menu = (H_HMENU)load_menu_internal(proc, s_classes[k].menu_id, is_unicode);
                }
                break;
            }
        }
    }
    if (dlg_menu_ord != 0 && !dlg->menu) {
        dlg->menu = (H_HMENU)load_menu_internal(proc, dlg_menu_ord, false);
    }

    /* Font info if DS_SETFONT (0x40) or DS_SHELLFONT (0x48) */
    if (style & 0x40) {
        if (is_ex) {
            if (p + 6 <= end) p += 6; /* pointsize, weight, italic, charset */
        } else {
            if (p + 2 <= end) p += 2; /* pointsize */
        }
        p = skip_dlg_sz_or_ord(p, end, NULL, 0, NULL); /* typeface */
    }

    int px_w = (dcx * 7) / 4 + 12;
    int px_h = (dcy * 15) / 8 + 28;
    if (px_w < 120) px_w = 240;
    if (px_h < 80)  px_h = 160;
    if (px_w > DEFAULT_SCREEN_W) px_w = DEFAULT_SCREEN_W - 20;
    if (px_h > DEFAULT_SCREEN_H) px_h = DEFAULT_SCREEN_H - 20;

    dlg->style = style | WS_VISIBLE;
    dlg->ex_style = ex_style;
    dlg->width = px_w;
    dlg->height = px_h;
    dlg->x = (dx > 0 && dx < 600) ? ((dx * 7) / 4) : ((DEFAULT_SCREEN_W - px_w) / 2);
    dlg->y = (dy > 0 && dy < 400) ? ((dy * 15) / 8) : ((DEFAULT_SCREEN_H - px_h) / 2);
    dlg->hdc = gdi_create_window_dc(hwndDlg);
    dlg->fb_stride = px_w * 4;
    dlg->framebuffer = allocate_window_framebuffer(px_w, px_h, 0xFFC0C0C0);
    gdi_dc_t *dc = gdi_get_dc(dlg->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        if (bm_obj) {
            bm_obj->bitmap.bpp = 32;
            dc->selected_bitmap = (H_HBITMAP)hbm;
        }
    }
    update_window_client_area(dlg);
    paint_window_decorations(dlg);

    LOGI("CreateDialog(%s): '%s' -> HWND 0x%04X [%d,%d %dx%d, %u items]",
         is_ex ? "EX" : "STD", dlg->title, hwndDlg, dlg->x, dlg->y, dlg->width, dlg->height, cdit);

    H_HWND first_ctrl = 0;
    for (uint16_t idx = 0; idx < cdit && p < end; idx++) {
        uintptr_t rel = (uintptr_t)(p - data);
        if (rel & 3) p += (4 - (rel & 3));
        if (p >= end) break;

        uint32_t c_style = 0, c_ex_style = 0, c_id = 0;
        int16_t cx = 0, cy = 0, ccx = 0, ccy = 0;

        if (is_ex) {
            if (p + 24 > end) break;
            c_ex_style = *(const uint32_t *)(p + 4);
            c_style    = *(const uint32_t *)(p + 8);
            cx         = *(const int16_t *)(p + 12);
            cy         = *(const int16_t *)(p + 14);
            ccx        = *(const int16_t *)(p + 16);
            ccy        = *(const int16_t *)(p + 18);
            c_id       = *(const uint32_t *)(p + 20);
            p += 24;
        } else {
            if (p + 18 > end) break;
            c_style    = *(const uint32_t *)(p + 0);
            c_ex_style = *(const uint32_t *)(p + 4);
            cx         = *(const int16_t *)(p + 8);
            cy         = *(const int16_t *)(p + 10);
            ccx        = *(const int16_t *)(p + 12);
            ccy        = *(const int16_t *)(p + 14);
            c_id       = *(const uint16_t *)(p + 16);
            p += 18;
        }

        char c_class[64] = {0};
        uint16_t c_class_ord = 0;
        p = skip_dlg_sz_or_ord(p, end, c_class, sizeof(c_class), &c_class_ord);
        if (c_class_ord != 0) {
            switch (c_class_ord) {
                case 0x0080: strcpy(c_class, "Button"); break;
                case 0x0081: strcpy(c_class, "Edit"); break;
                case 0x0082: strcpy(c_class, "Static"); break;
                case 0x0083: strcpy(c_class, "ListBox"); break;
                case 0x0084: strcpy(c_class, "ScrollBar"); break;
                case 0x0085: strcpy(c_class, "ComboBox"); break;
                default:     strcpy(c_class, "Static"); break;
            }
        }

        char c_title[128] = {0};
        uint16_t c_title_ord = 0;
        p = skip_dlg_sz_or_ord(p, end, c_title, sizeof(c_title), &c_title_ord);

        /* Extra creation data */
        if (p + 2 <= end) {
            uint16_t extra_bytes = *(const uint16_t *)p;
            p += 2;
            if (extra_bytes > 0 && p + extra_bytes <= end) p += extra_bytes;
        }

        int c_slot = -1;
        for (int i = 1; i < MAX_WINDOWS; i++) {
            if (!s_windows[i].in_use) { c_slot = i; break; }
        }
        if (c_slot != -1) {
            H_HWND hctrl = (H_HWND)(0x4000 + c_slot);
            win32_window_t *cw = &s_windows[c_slot];
            memset(cw, 0, sizeof(win32_window_t));
            cw->in_use = true;
            cw->hwnd = hctrl;
            cw->parent = hwndDlg;
            cw->style = c_style | WS_CHILD;
            cw->ex_style = c_ex_style;
            cw->menu = (H_HMENU)c_id;
            cw->x = (cx * 7) / 4;
            cw->y = (cy * 15) / 8;
            cw->width = (ccx * 7) / 4;
            cw->height = (ccy * 15) / 8;
            if (cw->width < 8) cw->width = 24;
            if (cw->height < 8) cw->height = 16;
            cw->visible = (c_style & WS_VISIBLE) != 0;
            cw->enabled = !(c_style & 0x08000000U /* WS_DISABLED */);
            strncpy(cw->class_name, c_class, sizeof(cw->class_name) - 1);
            strncpy(cw->title, c_title, sizeof(cw->title) - 1);

            for (int k = 0; k < s_class_count; k++) {
                if (strcasecmp(s_classes[k].class_name, cw->class_name) == 0) {
                    cw->wndproc = s_classes[k].wndproc;
                    break;
                }
            }

            cw->hdc = gdi_create_window_dc(hctrl);
            cw->fb_stride = cw->width * 4;
            uint32_t bg_c = (strcasecmp(cw->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
            cw->framebuffer = allocate_window_framebuffer(cw->width, cw->height, bg_c);
            gdi_dc_t *cdc = gdi_get_dc(cw->hdc);
            if (cdc) {
                H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
                gdi_object_t *bm_obj = gdi_get_obj(hbm);
                if (bm_obj) {
                    bm_obj->bitmap.bpp = 32;
                    cdc->selected_bitmap = (H_HBITMAP)hbm;
                }
            }
            update_window_client_area(cw);
            if (cw->wndproc >= 0xFE010000U && cw->wndproc <= 0xFE0100FFU) {
                builtin_wndproc_dispatch(proc, cw, cw->wndproc, WM_CREATE, 0, 0);
            } else {
                paint_window_decorations(cw);
            }
            if (!first_ctrl) first_ctrl = hctrl;
        }
    }

    s_active_hwnd = hwndDlg;
    s_windows_dirty = true;
    if (dlgProc) {
        call_wndproc(proc, dlgProc, hwndDlg, 0x0110 /* WM_INITDIALOG */, (WPARAM)first_ctrl, dwInitParam);
    } else if (dlg->wndproc && dlg->wndproc < 0xFE000000U) {
        call_wndproc(proc, dlg->wndproc, hwndDlg, WM_CREATE, 0, 0);
        call_wndproc(proc, dlg->wndproc, hwndDlg, 0x0110 /* WM_INITDIALOG */, (WPARAM)first_ctrl, dwInitParam);
    }
    return (uint32_t)hwndDlg;
}

static uint32_t u32_CreateDialogParamW(win32_process_t *proc, uint32_t *args) {
    return create_dialog_from_template(proc, args[1], (H_HWND)args[2], args[3], args[4], true, false);
}

static uint32_t u32_CreateDialogParamA(win32_process_t *proc, uint32_t *args) {
    return create_dialog_from_template(proc, args[1], (H_HWND)args[2], args[3], args[4], false, false);
}

static uint32_t u32_CreateDialogIndirectParamW(win32_process_t *proc, uint32_t *args) {
    return create_dialog_from_template(proc, args[1], (H_HWND)args[2], args[3], args[4], true, true);
}

static uint32_t u32_CreateDialogIndirectParamA(win32_process_t *proc, uint32_t *args) {
    return create_dialog_from_template(proc, args[1], (H_HWND)args[2], args[3], args[4], false, true);
}

static uint32_t u32_DialogBoxParamW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1; /* IDOK */
}

static uint32_t u32_DialogBoxParamA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1; /* IDOK */
}

static uint32_t u32_TranslateAcceleratorW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_wsprintfW(win32_process_t *proc, uint32_t *args) {
    uint32_t out_va = args[0];
    uint32_t fmt_va = args[1];
    if (!out_va || !fmt_va) return 0;

    uint16_t fmt_u16[256];
    int idx = 0;
    while (idx < 255) {
        uint16_t ch = 0;
        if (!guest_mem_read(&proc->mem, fmt_va + idx * 2, &ch, 2) || ch == 0) break;
        fmt_u16[idx++] = ch;
    }
    fmt_u16[idx] = 0;

    char fmt_ascii[256];
    for (int i = 0; i <= idx; i++) {
        fmt_ascii[i] = (char)fmt_u16[i];
    }

    char out_ascii[512];
    snprintf(out_ascii, sizeof(out_ascii), fmt_ascii, args[2], args[3], args[4], args[5]);

    int out_len = strlen(out_ascii);
    for (int i = 0; i <= out_len; i++) {
        uint16_t ch = (uint16_t)(uint8_t)out_ascii[i];
        guest_mem_write(&proc->mem, out_va + i * 2, &ch, 2);
    }
    return (uint32_t)out_len;
}

static uint32_t u32_DefFrameProcW(win32_process_t *proc, uint32_t *args) {
    uint32_t f_args[4] = { args[0], args[2], args[3], args[4] };
    return u32_DefWindowProcA(proc, f_args);
}
static uint32_t u32_DefMDIChildProcW(win32_process_t *proc, uint32_t *args) {
    return u32_DefWindowProcA(proc, args);
}
static uint32_t u32_GetWindowLongW(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = args[0];
    int index = (int)args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (!w) return 0;
    if (index >= 0 && index < 64) return w->extra_bytes[index / 4];
    if (index == -16) return w->style;      /* GWL_STYLE */
    if (index == -20) return w->ex_style;   /* GWL_EXSTYLE */
    if (index == -4)  return w->wndproc;    /* GWL_WNDPROC */
    if (index == -21) return w->user_data;  /* GWL_USERDATA */
    return 0;
}
static uint32_t u32_SetWindowLongW(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = args[0];
    int index = (int)args[1];
    uint32_t val = args[2];
    win32_window_t *w = user_get_window(hwnd);
    if (!w) return 0;
    uint32_t old = 0;
    if (index >= 0 && index < 64) {
        old = w->extra_bytes[index / 4];
        w->extra_bytes[index / 4] = val;
    }
    else if (index == -16) { old = w->style; w->style = val; }
    else if (index == -20) { old = w->ex_style; w->ex_style = val; }
    else if (index == -4)  { old = w->wndproc; w->wndproc = val; }
    else if (index == -21) { old = w->user_data; w->user_data = val; }
    return old;
}
static uint32_t u32_SetWindowPos(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = args[0];
    int x = (int)args[2];
    int y = (int)args[3];
    int cx = (int)args[4];
    int cy = (int)args[5];
    uint32_t flags = args[6];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        if (!(flags & 0x0002)) { w->x = x; w->y = y; }
        if (!(flags & 0x0001) && cx > 0 && cy > 0) {
            if (w->width != cx || w->height != cy) {
                w->width = cx;
                w->height = cy;
                w->fb_stride = cx * 4;
                if (w->framebuffer) free(w->framebuffer);
                uint32_t bg_col = (strcasecmp(w->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
                w->framebuffer = allocate_window_framebuffer(cx, cy, bg_col);
                update_window_client_area(w);
                if (w->wndproc >= 0xFE010000U && w->wndproc <= 0xFE0100FFU) {
                    paint_builtin_control(w, w->wndproc);
                } else {
                    paint_window_decorations(w);
                }
                w->needs_redraw = true;
                s_windows_dirty = true;
                user_post_message(hwnd, WM_PAINT, 0, 0);
            }
        }
        if (flags & 0x0040) w->visible = true;
        if (flags & 0x0080) w->visible = false;
        w->needs_redraw = true;
        return 1;
    }
    return 0;
}
static uint32_t u32_SetWindowTextA(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = args[0];
    const char *title = (const char *)guest_va_to_host(&proc->mem, args[1]);
    win32_window_t *w = user_get_window(hwnd);
    if (w && title) {
        strncpy(w->title, title, sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
        paint_window_decorations(w);
        w->needs_redraw = true;
        return 1;
    }
    return 0;
}
static uint32_t u32_SetWindowTextW(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = args[0];
    uint32_t wtitle_va = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w && wtitle_va) {
        uint16_t wbuf[128] = {0};
        guest_mem_read(&proc->mem, wtitle_va, wbuf, sizeof(wbuf));
        wchar_to_ascii(wbuf, w->title, sizeof(w->title));
        paint_window_decorations(w);
        w->needs_redraw = true;
        return 1;
    }
    return 0;
}
static uint32_t u32_MonitorFromWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00010001; /* HMONITOR */
}
static uint32_t u32_GetWindowTextA(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = args[0];
    char *out_buf = (char *)guest_va_to_host(&proc->mem, args[1]);
    int max_len = (int)args[2];
    win32_window_t *w = user_get_window(hwnd);
    if (w && out_buf && max_len > 0) {
        strncpy(out_buf, w->title, max_len - 1);
        out_buf[max_len - 1] = '\0';
        return (uint32_t)strlen(out_buf);
    }
    return 0;
}
static uint32_t u32_GetClientRect(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = args[0];
    uint32_t rect_va = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w && rect_va) {
        int32_t rect[4] = { 0, 0, w->client_w, w->client_h };
        guest_mem_write(&proc->mem, rect_va, rect, sizeof(rect));
        return 1;
    }
    return 0;
}
static uint32_t u32_DrawIcon(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    H_HICON hicon = (H_HICON)args[3];

    if (hicon <= 0 || hicon >= MAX_ICONS || !s_icons[hicon].in_use) return 0;

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;

    gdi_object_t *bm_obj = gdi_get_obj(dc->selected_bitmap);
    if (!bm_obj || bm_obj->type != GDI_OBJ_BITMAP || !bm_obj->bitmap.bits) return 0;

    uint8_t *bits = bm_obj->bitmap.bits;
    int bw = bm_obj->bitmap.width;
    int bh = bm_obj->bitmap.height;
    int stride = bm_obj->bitmap.stride;

    uint32_t *icon_px = s_icons[hicon].pixels;

    for (int dy = 0; dy < 32; dy++) {
        int py = y + dy;
        if (py < 0 || py >= bh) continue;
        for (int dx = 0; dx < 32; dx++) {
            int px = x + dx;
            if (px < 0 || px >= bw) continue;
            uint32_t color = icon_px[dy * 32 + dx];
            uint8_t a = (color >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                *(uint32_t *)(bits + py * stride + px * 4) = color;
            } else {
                uint32_t *dst = (uint32_t *)(bits + py * stride + px * 4);
                uint8_t dr = (*dst >> 16) & 0xFF;
                uint8_t dg = (*dst >> 8) & 0xFF;
                uint8_t db = *dst & 0xFF;

                uint8_t sr = (color >> 16) & 0xFF;
                uint8_t sg = (color >> 8) & 0xFF;
                uint8_t sb = color & 0xFF;

                uint8_t out_r = (uint8_t)((sr * a + dr * (255 - a)) / 255);
                uint8_t out_g = (uint8_t)((sg * a + dg * (255 - a)) / 255);
                uint8_t out_b = (uint8_t)((sb * a + db * (255 - a)) / 255);

                *dst = (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }
    return 1;
}
static uint32_t u32_CreateIcon(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t u32_GetIconInfo(win32_process_t *proc, uint32_t *args) {
    uint32_t out_va = args[1];
    if (out_va) {
        uint32_t dummy_info[5] = { 1, 16, 16, 1, 1 };
        guest_mem_write(&proc->mem, out_va, dummy_info, sizeof(dummy_info));
        return 1;
    }
    return 0;
}
static uint32_t u32_ArrangeIconicWindows(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 32;
}
static uint32_t u32_IsIconic(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    win32_window_t *w = user_get_window(args[0]);
    return w ? (uint32_t)w->minimized : 0;
}
static uint32_t u32_IsZoomed(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    win32_window_t *w = user_get_window(args[0]);
    return w ? (uint32_t)w->maximized : 0;
}
static uint32_t u32_CloseWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    win32_window_t *w = user_get_window(args[0]);
    if (w) { w->minimized = true; w->needs_redraw = true; return 1; }
    return 0;
}
static uint32_t u32_OpenIcon(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    win32_window_t *w = user_get_window(args[0]);
    if (w) { w->minimized = false; w->needs_redraw = true; return 1; }
    return 0;
}
static int alloc_menu_slot(void) {
    for (int i = 1; i < MAX_MENUS; i++) {
        if (!s_menus[i].in_use) {
            memset(&s_menus[i], 0, sizeof(win32_menu_t));
            s_menus[i].in_use = true;
            s_menus[i].hmenu = (H_HMENU)(0x5000 + i);
            return i;
        }
    }
    return -1;
}

static const uint8_t *parse_menu_v0_items(const uint8_t *p, const uint8_t *end, win32_menu_t *menu) {
    while (p + 2 <= end) {
        uint16_t flags = *(const uint16_t *)p;
        p += 2;
        uint16_t item_id = 0;
        if (!(flags & 0x10 /* MF_POPUP */)) {
            if (p + 2 > end) break;
            item_id = *(const uint16_t *)p;
            p += 2;
        }

        char title[48] = {0};
        int ti = 0;
        while (p + 2 <= end) {
            uint16_t ch = *(const uint16_t *)p;
            p += 2;
            if (ch == 0) break;
            if (ch == '\t') {
                /* Skip accelerator shortcut after tab or keep space */
                if (ti < 46) title[ti++] = ' ';
                continue;
            }
            if (ti < 47 && ch != '&') title[ti++] = wide_char_to_ascii(ch);
        }
        title[ti] = '\0';

        win32_menu_item_t *item = NULL;
        if (menu && menu->item_count < MAX_MENU_ITEMS) {
            item = &menu->items[menu->item_count++];
            strncpy(item->title, title, sizeof(item->title) - 1);
            item->flags = flags;
            item->id = item_id;
        }

        if (flags & 0x10 /* MF_POPUP */) {
            int sub_slot = alloc_menu_slot();
            win32_menu_t *sub_menu = (sub_slot != -1) ? &s_menus[sub_slot] : NULL;
            if (item && sub_menu) {
                item->hsub = sub_menu->hmenu;
            }
            p = parse_menu_v0_items(p, end, sub_menu);
        }

        if (flags & 0x80 /* MF_END */) break;
    }
    return p;
}

static const uint8_t *parse_menu_v1_items(const uint8_t *base, const uint8_t *p, const uint8_t *end, win32_menu_t *menu) {
    while (p < end) {
        uintptr_t rel = (uintptr_t)(p - base);
        if (rel & 3) p += (4 - (rel & 3));
        if (p + 14 > end) break;

        uint32_t dwType  = *(const uint32_t *)(p + 0);
        uint32_t dwState = *(const uint32_t *)(p + 4);
        uint32_t menuId  = *(const uint32_t *)(p + 8);
        uint16_t bResInfo = *(const uint16_t *)(p + 12);
        p += 14;

        char title[48] = {0};
        int ti = 0;
        while (p + 2 <= end) {
            uint16_t ch = *(const uint16_t *)p;
            p += 2;
            if (ch == 0) break;
            if (ch == '\t') {
                if (ti < 46) title[ti++] = ' ';
                continue;
            }
            if (ti < 47 && ch != '&') title[ti++] = wide_char_to_ascii(ch);
        }
        title[ti] = '\0';

        win32_menu_item_t *item = NULL;
        if (menu && menu->item_count < MAX_MENU_ITEMS) {
            item = &menu->items[menu->item_count++];
            if (dwType & 0x0800 /* MFT_SEPARATOR */) {
                strcpy(item->title, "---");
                item->flags = 0x0800;
            } else {
                strncpy(item->title, title, sizeof(item->title) - 1);
                item->flags = dwState;
            }
            item->id = menuId;
        }

        if (bResInfo & 0x01 /* Pop-up submenu */) {
            uintptr_t r2 = (uintptr_t)(p - base);
            if (r2 & 3) p += (4 - (r2 & 3));
            if (p + 4 <= end) p += 4; /* Skip dwHelpId */

            int sub_slot = alloc_menu_slot();
            win32_menu_t *sub_menu = (sub_slot != -1) ? &s_menus[sub_slot] : NULL;
            if (item && sub_menu) {
                item->hsub = sub_menu->hmenu;
            }
            p = parse_menu_v1_items(base, p, end, sub_menu);
        }

        if (bResInfo & 0x80 /* Last item in level */) break;
    }
    return p;
}

static uint32_t load_menu_internal(win32_process_t *proc, uint32_t menu_id_or_name, bool is_unicode) {
    uint32_t data_sz = 0;
    uint32_t menu_va = pe_find_resource_ex(proc, 4 /* RT_MENU */, menu_id_or_name, is_unicode, &data_sz);
    if (!menu_va) {
        menu_va = pe_find_resource_ex(proc, 4, 1, false, &data_sz);
        if (!menu_va) menu_va = pe_find_resource_ex(proc, 4, 0, false, &data_sz);
    }

    int root_slot = alloc_menu_slot();
    if (root_slot == -1) return 0x5001;
    win32_menu_t *root_menu = &s_menus[root_slot];

    if (!menu_va || data_sz < 4) {
        return (uint32_t)root_menu->hmenu;
    }

    const uint8_t *menu_data = guest_va_to_host(&proc->mem, menu_va);
    if (!menu_data) return (uint32_t)root_menu->hmenu;

    uint16_t version = *(const uint16_t *)(menu_data + 0);
    uint16_t offset  = *(const uint16_t *)(menu_data + 2);
    const uint8_t *end = menu_data + data_sz;

    if (version == 1) {
        const uint8_t *p = menu_data + 4 + offset;
        parse_menu_v1_items(menu_data, p, end, root_menu);
    } else {
        const uint8_t *p = menu_data + 4 + offset;
        parse_menu_v0_items(p, end, root_menu);
    }

    LOGI("load_menu_internal(v%u): Loaded menu 0x%04X with %d top-level items",
         version, root_menu->hmenu, root_menu->item_count);
    return (uint32_t)root_menu->hmenu;
}

static uint32_t u32_LoadMenuW(win32_process_t *proc, uint32_t *args) {
    return load_menu_internal(proc, args[1], true);
}

static uint32_t u32_LoadMenuA(win32_process_t *proc, uint32_t *args) {
    return load_menu_internal(proc, args[1], false);
}

static uint32_t u32_GetSubMenu(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HMENU hmenu = (H_HMENU)args[0];
    int pos = (int)args[1];
    int slot = (int)(hmenu - 0x5000);
    if (slot >= 1 && slot < MAX_MENUS && s_menus[slot].in_use) {
        if (pos >= 0 && pos < s_menus[slot].item_count) {
            if (s_menus[slot].items[pos].hsub != 0) {
                return (uint32_t)s_menus[slot].items[pos].hsub;
            }
        }
    }
    return (args[0] * 16) + args[1] + 1;
}
static uint32_t u32_EnableMenuItem(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t u32_DestroyMenu(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t u32_CheckDlgButton(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t u32_IsDlgButtonChecked(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}
static uint32_t u32_CheckRadioButton(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t u32_SendDlgItemMessageA(win32_process_t *proc, uint32_t *args) {
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, args);
    if (hctrl) {
        return handle_send_message(proc, hctrl, args[2], args[3], args[4], false);
    }
    return 0;
}
static uint32_t u32_SendDlgItemMessageW(win32_process_t *proc, uint32_t *args) {
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, args);
    if (hctrl) {
        return handle_send_message(proc, hctrl, args[2], args[3], args[4], true);
    }
    return 0;
}
static uint32_t u32_SetDlgItemTextA(win32_process_t *proc, uint32_t *args) {
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, args);
    if (hctrl) {
        uint32_t st_args[2] = { hctrl, args[2] };
        return u32_SetWindowTextA(proc, st_args);
    }
    return 1;
}
static uint32_t u32_GetDlgItemTextA(win32_process_t *proc, uint32_t *args) {
    H_HWND hctrl = (H_HWND)u32_GetDlgItem(proc, args);
    if (hctrl) {
        uint32_t gt_args[3] = { hctrl, args[2], args[3] };
        return u32_GetWindowTextA(proc, gt_args);
    }
    char *out_buf = (char *)guest_va_to_host(&proc->mem, args[2]);
    uint32_t max_sz = args[3];
    if (out_buf && max_sz > 0) {
        out_buf[0] = '\0';
    }
    return 0;
}

static uint32_t u32_LoadStringA(win32_process_t *proc, uint32_t *args) {
    char *buf = (char *)guest_va_to_host(&proc->mem, args[2]);
    uint32_t max_len = args[3];
    uint32_t id = args[1];
    if (buf && max_len > 0) {
        uint16_t wbuf[256] = {0};
        int len = pe_load_resource_string(proc, id, wbuf, 255);
        if (len > 0) {
            int copied = (len < (int)max_len - 1) ? len : (int)max_len - 1;
            for (int i = 0; i < copied; i++) {
                buf[i] = (char)wbuf[i];
            }
            buf[copied] = '\0';
            return (uint32_t)copied;
        }
        snprintf(buf, max_len, "String #%u", id);
        return (uint32_t)strlen(buf);
    }
    return 0;
}

static uint32_t u32_CallWindowProcW(win32_process_t *proc, uint32_t *args) {
    GUEST_PTR prev_proc = args[0];
    H_HWND hwnd = (H_HWND)args[1];
    UINT msg = args[2];
    WPARAM wParam = args[3];
    LPARAM lParam = args[4];
    return (uint32_t)call_wndproc(proc, prev_proc, hwnd, msg, wParam, lParam);
}

static uint32_t u32_CreateStatusWindowW(win32_process_t *proc, uint32_t *args) {
    uint32_t style = args[0];
    uint32_t lpszText = args[1];
    H_HWND hwndParent = (H_HWND)args[2];
    uint32_t wID = args[3];
    int slot = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!s_windows[i].in_use) { slot = i; break; }
    }
    if (slot == -1) return 0;
    H_HWND hwnd = (H_HWND)(0x4000 + slot);
    win32_window_t *w = &s_windows[slot];
    memset(w, 0, sizeof(*w));
    w->in_use = true;
    w->hwnd = hwnd;
    w->parent = hwndParent;
    w->style = style | WS_CHILD | WS_VISIBLE;
    w->menu = (H_HMENU)wID;
    w->visible = (style & WS_VISIBLE) != 0;
    w->enabled = true;
    w->wndproc = BUILTIN_WNDPROC_STATUSBAR;
    strcpy(w->class_name, "msctls_statusbar32");
    if (lpszText) {
        uint16_t wbuf[128] = {0};
        guest_mem_read(&proc->mem, lpszText, wbuf, sizeof(wbuf) - 2);
        wchar_to_ascii(wbuf, w->title, sizeof(w->title));
        strncpy(w->sb_texts[0], w->title, sizeof(w->sb_texts[0]) - 1);
    }
    win32_window_t *pw = user_get_window(hwndParent);
    w->width = pw ? pw->client_w : 640;
    w->height = 22;
    w->x = 0;
    w->y = pw ? (pw->client_h - 22) : 400;
    w->hdc = gdi_create_window_dc(hwnd);
    w->fb_stride = w->width * 4;
    w->framebuffer = allocate_window_framebuffer(w->width, w->height, 0xFFC0C0C0);
    gdi_dc_t *dc = gdi_get_dc(w->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        if (bm_obj) {
            bm_obj->bitmap.bpp = 32;
            bm_obj->bitmap.is_window_fb = true;
            dc->selected_bitmap = (H_HBITMAP)hbm;
        }
    }
    update_window_client_area(w);
    paint_builtin_control(w, w->wndproc);
    /* If statusbar is initially visible, shrink any sibling Edit control filling the parent client area so it doesn't cover the statusbar */
    if (w->visible && pw) {
        for (int i = 1; i < MAX_WINDOWS; i++) {
            win32_window_t *sib = &s_windows[i];
            if (sib->in_use && sib != w && sib->parent == hwndParent && sib->height > w->y) {
                sib->height = w->y;
                if (sib->framebuffer) free(sib->framebuffer);
                uint32_t bg_c = (strcasecmp(sib->class_name, "Edit") == 0) ? 0xFFFFFFFF : 0xFFC0C0C0;
                sib->framebuffer = allocate_window_framebuffer(sib->width, sib->height, bg_c);
                update_window_client_area(sib);
                paint_builtin_control(sib, sib->wndproc);
            }
        }
    }
    return (uint32_t)hwnd;
}

static uint32_t u32_CharUpperW(win32_process_t *proc, uint32_t *args) {
    uint32_t ptr = args[0];
    if ((ptr & 0xFFFF0000) == 0) {
        uint16_t ch = (uint16_t)ptr;
        if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
        return (uint32_t)ch;
    }
    uint16_t *str = (uint16_t *)guest_va_to_host(&proc->mem, ptr);
    if (str) {
        while (*str) {
            if (*str >= 'a' && *str <= 'z') *str -= ('a' - 'A');
            str++;
        }
    }
    return ptr;
}

static uint32_t u32_CharNextW(win32_process_t *proc, uint32_t *args) {
    uint32_t ptr = args[0];
    if (!ptr) return 0;
    uint16_t ch = 0;
    guest_mem_read(&proc->mem, ptr, &ch, 2);
    return ch ? (ptr + 2) : ptr;
}

static uint32_t u32_SetCursor(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00010001; /* HCURSOR */
}

static uint32_t u32_GetFocus(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return (uint32_t)s_active_hwnd;
}

static uint32_t u32_GetForegroundWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return (uint32_t)s_active_hwnd;
}

static uint32_t u32_IsWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    return (w && w->in_use) ? 1 : 0;
}

static uint32_t u32_GetWindowPlacement(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    uint32_t lpwndpl = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w && lpwndpl) {
        uint32_t pl[11] = {0};
        pl[0] = 44;
        pl[1] = 0;
        pl[2] = 1; /* SW_SHOWNORMAL */
        pl[7] = w->x;
        pl[8] = w->y;
        pl[9] = w->x + w->width;
        pl[10] = w->y + w->height;
        guest_mem_write(&proc->mem, lpwndpl, pl, sizeof(pl));
        return 1;
    }
    return 0;
}

static uint32_t u32_SetWindowPlacement(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_GetWindowTextW(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    uint32_t out_va = args[1];
    int max_count = (int)args[2];
    win32_window_t *w = user_get_window(hwnd);
    if (!w || !out_va || max_count <= 0) return 0;
    int len = (int)strlen(w->title);
    if (len >= max_count) len = max_count - 1;
    uint16_t wbuf[128] = {0};
    for (int i = 0; i < len; i++) wbuf[i] = (uint8_t)w->title[i];
    wbuf[len] = 0;
    guest_mem_write(&proc->mem, out_va, wbuf, (len + 1) * sizeof(uint16_t));
    return len;
}

static uint32_t u32_GetWindowTextLengthW(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    return w ? (uint32_t)strlen(w->title) : 0;
}

static uint32_t u32_GetSystemMenu(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00020001; /* HMENU */
}

static uint32_t u32_SetWinEventHook(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00010001; /* HWINEVENTHOOK */
}

static uint32_t u32_UnhookWinEvent(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_IsDialogMessageW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* Not a dialog message */
}

static uint32_t u32_DispatchMessageW(win32_process_t *proc, uint32_t *args) {
    return u32_DispatchMessageA(proc, args);
}

static uint32_t u32_GetMessageW(win32_process_t *proc, uint32_t *args) {
    return u32_GetMessageA(proc, args);
}

static uint32_t u32_PeekMessageW(win32_process_t *proc, uint32_t *args) {
    return u32_PeekMessageA(proc, args);
}

static uint32_t u32_RedrawWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        w->needs_redraw = true;
        s_windows_dirty = true;
        user_post_message(hwnd, WM_PAINT, 0, 0);
    }
    return 1;
}

static uint32_t u32_SetScrollPos(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    return args[2]; /* Return previous pos */
}

static uint32_t u32_GetDlgCtrlID(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    return w ? (uint32_t)(uintptr_t)w->menu : 0;
}

static uint32_t u32_GetKeyboardLayout(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x04090409; /* US English HKL */
}

static uint32_t u32_TrackMouseEvent(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_DestroyIcon(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_MessageBeep(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_GetPropW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_SetPropW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_RemovePropW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_OpenClipboard(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_IsClipboardFormatAvailable(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_CloseClipboard(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_DrawFocusRect(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_DrawTextExW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 16;
}

static uint32_t u32_NotifyWinEvent(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_FillRect(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t lprc_va = args[1];
    H_HBRUSH hbr = (H_HBRUSH)args[2];
    WIN_RECT rc;
    if (!guest_mem_read(&proc->mem, lprc_va, &rc, sizeof(rc))) return 0;
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

    uint32_t color = 0xFFC0C0C0;
    if ((uint32_t)hbr <= 32) {
        if ((uint32_t)hbr == 6) color = 0xFFFFFFFF; /* COLOR_WINDOW */
        else if ((uint32_t)hbr == 16) color = 0xFFC0C0C0; /* COLOR_BTNFACE */
    } else {
        gdi_object_t *br = gdi_get_obj((H_HANDLE)hbr);
        if (br && br->type == GDI_OBJ_BRUSH) color = br->brush.color | 0xFF000000;
    }

    for (int y = rc.top; y < rc.bottom; y++) {
        if (y < 0 || y >= bm->bitmap.height) continue;
        for (int x = rc.left; x < rc.right; x++) {
            if (x < 0 || x >= bm->bitmap.width) continue;
            *(uint32_t *)(bm->bitmap.bits + (y * bm->bitmap.stride) + (x * 4)) = color;
        }
    }
    return 1;
}

static uint32_t dummy_ShellAboutW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t dummy_PlaySoundW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t dummy_InitCommonControlsEx(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_AdjustWindowRect(win32_process_t *proc, uint32_t *args) {
    uint32_t rect_va = args[0];
    uint32_t style = args[1];
    uint32_t has_menu = args[2];
    if (!rect_va) return 0;
    int32_t rc[4];
    if (!guest_mem_read(&proc->mem, rect_va, rc, sizeof(rc))) return 0;
    int border = (style & (WS_BORDER | WS_THICKFRAME | WS_DLGFRAME)) ? 3 : 0;
    int caption = ((style & WS_CAPTION) == WS_CAPTION) ? 19 : 0;
    int menu = has_menu ? 20 : 0;
    rc[0] -= border;
    rc[1] -= (border + caption + menu);
    rc[2] += border;
    rc[3] += border;
    guest_mem_write(&proc->mem, rect_va, rc, sizeof(rc));
    return 1;
}

static uint32_t u32_GetSysColor(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int idx = (int)args[0];
    switch (idx) {
        case 1:  return 0x00808000; /* DESKTOP / BACKGROUND (Teal) */
        case 2:  return 0x00800000; /* ACTIVECAPTION (Dark blue) */
        case 3:  return 0x00808080; /* INACTIVECAPTION (Gray) */
        case 4:  return 0x00C0C0C0; /* MENU */
        case 5:  return 0x00FFFFFF; /* WINDOW (White) */
        case 6:  return 0x00000000; /* WINDOWFRAME (Black) */
        case 7:  return 0x00000000; /* MENUTEXT */
        case 8:  return 0x00000000; /* WINDOWTEXT */
        case 9:  return 0x00FFFFFF; /* CAPTIONTEXT */
        case 15: return 0x00C0C0C0; /* BTNFACE */
        case 16: return 0x00808080; /* BTNSHADOW */
        case 20: return 0x00FFFFFF; /* BTNHIGHLIGHT */
        default: return 0x00C0C0C0;
    }
}

static uint32_t u32_CopyRect(win32_process_t *proc, uint32_t *args) {
    uint32_t dst_va = args[0];
    uint32_t src_va = args[1];
    if (!dst_va || !src_va) return 0;
    int32_t rc[4];
    if (guest_mem_read(&proc->mem, src_va, rc, 16)) {
        guest_mem_write(&proc->mem, dst_va, rc, 16);
        return 1;
    }
    return 0;
}

static uint32_t u32_IntersectRect(win32_process_t *proc, uint32_t *args) {
    uint32_t dst_va = args[0];
    uint32_t s1_va = args[1];
    uint32_t s2_va = args[2];
    if (!dst_va || !s1_va || !s2_va) return 0;
    int32_t r1[4], r2[4], dst[4];
    guest_mem_read(&proc->mem, s1_va, r1, 16);
    guest_mem_read(&proc->mem, s2_va, r2, 16);
    dst[0] = r1[0] > r2[0] ? r1[0] : r2[0];
    dst[1] = r1[1] > r2[1] ? r1[1] : r2[1];
    dst[2] = r1[2] < r2[2] ? r1[2] : r2[2];
    dst[3] = r1[3] < r2[3] ? r1[3] : r2[3];
    if (dst[0] < dst[2] && dst[1] < dst[3]) {
        guest_mem_write(&proc->mem, dst_va, dst, 16);
        return 1;
    }
    memset(dst, 0, 16);
    guest_mem_write(&proc->mem, dst_va, dst, 16);
    return 0;
}

static uint32_t u32_InflateRect(win32_process_t *proc, uint32_t *args) {
    uint32_t rc_va = args[0];
    int dx = (int)args[1];
    int dy = (int)args[2];
    if (!rc_va) return 0;
    int32_t rc[4];
    if (guest_mem_read(&proc->mem, rc_va, rc, 16)) {
        rc[0] -= dx; rc[2] += dx;
        rc[1] -= dy; rc[3] += dy;
        guest_mem_write(&proc->mem, rc_va, rc, 16);
        return 1;
    }
    return 0;
}

static uint32_t u32_InvertRect(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t rc_va = args[1];
    if (!rc_va) return 0;
    int32_t rc[4];
    if (!guest_mem_read(&proc->mem, rc_va, rc, 16)) return 0;
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;
    for (int y = rc[1]; y < rc[3]; y++) {
        if (y < 0 || y >= bm->bitmap.height) continue;
        for (int x = rc[0]; x < rc[2]; x++) {
            if (x < 0 || x >= bm->bitmap.width) continue;
            uint32_t *p = (uint32_t *)(bm->bitmap.bits + y * bm->bitmap.stride + x * 4);
            *p = (*p ^ 0x00FFFFFF) | 0xFF000000;
        }
    }
    return 1;
}

static uint32_t u32_FrameRect(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t rc_va = args[1];
    H_HBRUSH hbr = (H_HBRUSH)args[2];
    if (!rc_va) return 0;
    int32_t rc[4];
    if (!guest_mem_read(&proc->mem, rc_va, rc, 16)) return 0;
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;
    uint32_t color = 0xFF000000;
    gdi_object_t *br_obj = gdi_get_obj((H_HANDLE)hbr);
    if (br_obj && br_obj->type == GDI_OBJ_BRUSH) color = br_obj->brush.color;
    for (int x = rc[0]; x < rc[2]; x++) {
        if (x >= 0 && x < bm->bitmap.width) {
            if (rc[1] >= 0 && rc[1] < bm->bitmap.height)
                *(uint32_t *)(bm->bitmap.bits + rc[1] * bm->bitmap.stride + x * 4) = color;
            if (rc[3] - 1 >= 0 && rc[3] - 1 < bm->bitmap.height)
                *(uint32_t *)(bm->bitmap.bits + (rc[3] - 1) * bm->bitmap.stride + x * 4) = color;
        }
    }
    for (int y = rc[1]; y < rc[3]; y++) {
        if (y >= 0 && y < bm->bitmap.height) {
            if (rc[0] >= 0 && rc[0] < bm->bitmap.width)
                *(uint32_t *)(bm->bitmap.bits + y * bm->bitmap.stride + rc[0] * 4) = color;
            if (rc[2] - 1 >= 0 && rc[2] - 1 < bm->bitmap.width)
                *(uint32_t *)(bm->bitmap.bits + y * bm->bitmap.stride + (rc[2] - 1) * 4) = color;
        }
    }
    return 1;
}

static uint32_t u32_ClientToScreen(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    uint32_t pt_va = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w && pt_va) {
        int sx = 0, sy = 0;
        get_window_screen_pos(w, &sx, &sy);
        int32_t pt[2];
        if (guest_mem_read(&proc->mem, pt_va, pt, sizeof(pt))) {
            pt[0] += (sx + w->client_x);
            pt[1] += (sy + w->client_y);
            guest_mem_write(&proc->mem, pt_va, pt, sizeof(pt));
            return 1;
        }
    }
    return 0;
}

static uint32_t u32_GetKeyState(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_SetFocus(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    H_HWND prev = s_active_hwnd;
    if (hwnd) s_active_hwnd = hwnd;
    return (uint32_t)prev;
}

static uint32_t u32_EnableWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    bool enable = (args[1] != 0);
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        bool prev = w->enabled;
        w->enabled = enable;
        return prev ? 0 : 1;
    }
    return 0;
}

static uint32_t u32_ShowCursor(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_GetCapture(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return (uint32_t)s_capture_hwnd;
}

static uint32_t u32_GetMenu(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w) return (uint32_t)w->menu;
    return 0;
}

static uint32_t u32_RegisterClassExW(win32_process_t *proc, uint32_t *args) {
    uint32_t wc_va = args[0];
    if (!wc_va || s_class_count >= MAX_CLASSES) return 0;

    uint32_t wndproc = 0, name_va = 0, icon = 0, cursor = 0, brush = 0, menu_va = 0;
    guest_mem_read(&proc->mem, wc_va + 8, &wndproc, 4);
    guest_mem_read(&proc->mem, wc_va + 24, &icon, 4);
    guest_mem_read(&proc->mem, wc_va + 28, &cursor, 4);
    guest_mem_read(&proc->mem, wc_va + 32, &brush, 4);
    guest_mem_read(&proc->mem, wc_va + 36, &menu_va, 4);
    guest_mem_read(&proc->mem, wc_va + 40, &name_va, 4);

    uint16_t wide_name[64] = {0};
    guest_mem_read(&proc->mem, name_va, wide_name, sizeof(wide_name));
    char ascii_name[64] = {0};
    wchar_to_ascii(wide_name, ascii_name, sizeof(ascii_name));

    strncpy(s_classes[s_class_count].class_name, ascii_name, sizeof(s_classes[0].class_name) - 1);
    s_classes[s_class_count].wndproc = wndproc;
    s_classes[s_class_count].icon = icon;
    s_classes[s_class_count].cursor = cursor;
    s_classes[s_class_count].background_brush = brush;
    s_classes[s_class_count].menu_id = menu_va;

    LOGI("RegisterClassExW: %s (WndProc=0x%08X, menu=0x%X)", ascii_name, wndproc, menu_va);
    s_class_count++;
    return 0x0001;
}

static uint32_t u32_RegisterClassExA(win32_process_t *proc, uint32_t *args) {
    uint32_t wc_va = args[0];
    if (!wc_va || s_class_count >= MAX_CLASSES) return 0;

    uint32_t wndproc = 0, name_va = 0, icon = 0, cursor = 0, brush = 0, menu_va = 0;
    guest_mem_read(&proc->mem, wc_va + 8, &wndproc, 4);
    guest_mem_read(&proc->mem, wc_va + 24, &icon, 4);
    guest_mem_read(&proc->mem, wc_va + 28, &cursor, 4);
    guest_mem_read(&proc->mem, wc_va + 32, &brush, 4);
    guest_mem_read(&proc->mem, wc_va + 36, &menu_va, 4);
    guest_mem_read(&proc->mem, wc_va + 40, &name_va, 4);

    const char *name = (const char *)guest_va_to_host(&proc->mem, name_va);
    if (!name) return 0;

    strncpy(s_classes[s_class_count].class_name, name, sizeof(s_classes[0].class_name) - 1);
    s_classes[s_class_count].wndproc = wndproc;
    s_classes[s_class_count].icon = icon;
    s_classes[s_class_count].cursor = cursor;
    s_classes[s_class_count].background_brush = brush;
    s_classes[s_class_count].menu_id = menu_va;

    LOGI("RegisterClassExA: %s (WndProc=0x%08X, menu=0x%X)", name, wndproc, menu_va);
    s_class_count++;
    return 0x0001;
}

static uint32_t u32_LoadImageW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x0001; /* Handle placeholder */
}

static uint32_t u32_RegisterWindowMessageW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    static uint32_t s_next_reg_msg = 0xC000;
    return s_next_reg_msg++;
}

static uint32_t u32_SetCursorPos(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    s_cursor_pos.x = (int)args[0];
    s_cursor_pos.y = (int)args[1];
    return 1;
}

static uint32_t u32_MsgWaitForMultipleObjects(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* WAIT_OBJECT_0 */
}

static uint32_t u32_GetWindowRect(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    uint32_t rect_va = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (!rect_va) return 0;
    WIN_RECT rc = { 0, 0, DEFAULT_SCREEN_W, DEFAULT_SCREEN_H };
    if (w) {
        int sx = w->x, sy = w->y;
        win32_window_t *pw = user_get_window(w->parent);
        if (pw) {
            sx += pw->x + pw->client_x;
            sy += pw->y + pw->client_y;
        }
        rc.left = sx;
        rc.top = sy;
        rc.right = sx + w->width;
        rc.bottom = sy + w->height;
    }
    guest_mem_write(&proc->mem, rect_va, &rc, sizeof(rc));
    return 1;
}

static uint32_t u32_ScreenToClient(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    uint32_t pt_va = args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (!pt_va) return 0;
    WIN_POINT pt = {0, 0};
    guest_mem_read(&proc->mem, pt_va, &pt, sizeof(pt));
    if (w) {
        int sx = w->x + w->client_x;
        int sy = w->y + w->client_y;
        win32_window_t *pw = user_get_window(w->parent);
        if (pw) {
            sx += pw->x + pw->client_x;
            sy += pw->y + pw->client_y;
        }
        pt.x -= sx;
        pt.y -= sy;
    }
    guest_mem_write(&proc->mem, pt_va, &pt, sizeof(pt));
    return 1;
}

static uint32_t u32_MapDialogRect(win32_process_t *proc, uint32_t *args) {
    uint32_t rect_va = args[1];
    if (!rect_va) return 0;
    WIN_RECT rc = {0};
    guest_mem_read(&proc->mem, rect_va, &rc, sizeof(rc));
    rc.left   = (rc.left * 7) / 4;
    rc.right  = (rc.right * 7) / 4;
    rc.top    = (rc.top * 15) / 8;
    rc.bottom = (rc.bottom * 15) / 8;
    guest_mem_write(&proc->mem, rect_va, &rc, sizeof(rc));
    return 1;
}

static uint32_t u32_GetSysColorBrush(win32_process_t *proc, uint32_t *args) {
    int idx = (int)args[0];
    uint32_t col = u32_GetSysColor(proc, args);
    if (idx >= 0 && idx < 32) {
        if (!s_sys_brushes[idx]) {
            H_HANDLE h = gdi_alloc_obj(GDI_OBJ_BRUSH);
            gdi_object_t *obj = gdi_get_obj(h);
            if (obj) obj->brush.color = col;
            s_sys_brushes[idx] = (H_HBRUSH)h;
        }
        return (uint32_t)s_sys_brushes[idx];
    }
    return 0;
}

static uint32_t u32_CharUpperA(win32_process_t *proc, uint32_t *args) {
    uint32_t ptr = args[0];
    if ((ptr & 0xFFFF0000) == 0) {
        uint8_t ch = (uint8_t)ptr;
        if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
        return (uint32_t)ch;
    }
    char *str = (char *)guest_va_to_host(&proc->mem, ptr);
    if (str) {
        while (*str) {
            if (*str >= 'a' && *str <= 'z') *str -= ('a' - 'A');
            str++;
        }
    }
    return ptr;
}

static uint32_t u32_DrawEdge(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t qrc_va = args[1];
    uint32_t edge = args[2];
    uint32_t grfFlags = args[3];
    if (!qrc_va) return 0;
    WIN_RECT rc = {0};
    guest_mem_read(&proc->mem, qrc_va, &rc, sizeof(rc));
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

    bool sunk = (edge & 0x0002 /* BDR_SUNKENOUTER */) || (edge & 0x0008 /* BDR_SUNKENINNER */);
    uint32_t c_tl_out = sunk ? 0xFF808080 : 0xFFFFFFFF;
    uint32_t c_br_out = sunk ? 0xFFFFFFFF : 0xFF000000;
    uint32_t c_tl_in  = sunk ? 0xFF000000 : 0xFFDFDFDF;
    uint32_t c_br_in  = sunk ? 0xFFDFDFDF : 0xFF808080;

    if (grfFlags & 0x0800 /* BF_MIDDLE */) {
        for (int y = rc.top; y < rc.bottom; y++) {
            if (y < 0 || y >= bm->bitmap.height) continue;
            uint32_t *row = (uint32_t *)(bm->bitmap.bits + y * bm->bitmap.stride);
            for (int x = rc.left; x < rc.right; x++) {
                if (x >= 0 && x < bm->bitmap.width) row[x] = 0xFFFFFFFF;
            }
        }
    }
    for (int x = rc.left; x < rc.right; x++) {
        if (x < 0 || x >= bm->bitmap.width) continue;
        if (rc.top >= 0 && rc.top < bm->bitmap.height)
            ((uint32_t *)(bm->bitmap.bits + rc.top * bm->bitmap.stride))[x] = c_tl_out;
        if (rc.top + 1 >= 0 && rc.top + 1 < bm->bitmap.height)
            ((uint32_t *)(bm->bitmap.bits + (rc.top + 1) * bm->bitmap.stride))[x] = c_tl_in;
        if (rc.bottom - 2 >= 0 && rc.bottom - 2 < bm->bitmap.height)
            ((uint32_t *)(bm->bitmap.bits + (rc.bottom - 2) * bm->bitmap.stride))[x] = c_br_in;
        if (rc.bottom - 1 >= 0 && rc.bottom - 1 < bm->bitmap.height)
            ((uint32_t *)(bm->bitmap.bits + (rc.bottom - 1) * bm->bitmap.stride))[x] = c_br_out;
    }
    for (int y = rc.top; y < rc.bottom; y++) {
        if (y < 0 || y >= bm->bitmap.height) continue;
        uint32_t *row = (uint32_t *)(bm->bitmap.bits + y * bm->bitmap.stride);
        if (rc.left >= 0 && rc.left < bm->bitmap.width) row[rc.left] = c_tl_out;
        if (rc.left + 1 >= 0 && rc.left + 1 < bm->bitmap.width) row[rc.left + 1] = c_tl_in;
        if (rc.right - 2 >= 0 && rc.right - 2 < bm->bitmap.width) row[rc.right - 2] = c_br_in;
        if (rc.right - 1 >= 0 && rc.right - 1 < bm->bitmap.width) row[rc.right - 1] = c_br_out;
    }
    if (grfFlags & 0x2000 /* BF_ADJUST */) {
        rc.left += 2; rc.top += 2; rc.right -= 2; rc.bottom -= 2;
        guest_mem_write(&proc->mem, qrc_va, &rc, sizeof(rc));
    }
    return 1;
}

static uint32_t u32_DrawFrameControl(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t lprc_va = args[1];
    /* uint32_t uType = args[2]; */
    uint32_t uState = args[3];
    uint32_t edge = (uState & 0x0200 /* DFCS_PUSHED */) ? 0x000A /* EDGE_SUNKEN */ : 0x0005 /* EDGE_RAISED */;
    uint32_t flags = 0x000F /* BF_RECT */ | 0x0800 /* BF_MIDDLE */ | ((uState & 0x2000) ? 0x2000 : 0);
    uint32_t edge_args[4] = { hdc, lprc_va, edge, flags };
    return u32_DrawEdge(proc, edge_args);
}

static uint32_t u32_TrackPopupMenuEx(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_GetClipboardData(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_CharNextA(win32_process_t *proc, uint32_t *args) {
    uint32_t ptr = args[0];
    if (!ptr) return 0;
    uint8_t ch = 0;
    guest_mem_read(&proc->mem, ptr, &ch, 1);
    return ch ? (ptr + 1) : ptr;
}

static uint32_t u32_CharPrevA(win32_process_t *proc, uint32_t *args) {
    uint32_t start = args[0];
    uint32_t cur = args[1];
    return (cur > start) ? (cur - 1) : start;
}

static uint32_t u32_GetCursorPos(win32_process_t *proc, uint32_t *args) {
    uint32_t pt_va = args[0];
    if (pt_va) guest_mem_write(&proc->mem, pt_va, &s_cursor_pos, sizeof(s_cursor_pos));
    return 1;
}

static uint32_t u32_DrawTextW(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t str_va = args[1];
    int len = (int)args[2];
    uint32_t rc_va = args[3];
    uint32_t format = args[4];
    if (!str_va || !rc_va) return 0;
    WIN_RECT rc = {0};
    guest_mem_read(&proc->mem, rc_va, &rc, sizeof(rc));
    if (len < 0) {
        uint16_t wch = 0;
        len = 0;
        while (guest_mem_read(&proc->mem, str_va + len * 2, &wch, 2) && wch != 0 && len < 255) len++;
    }
    if (len > 255) len = 255;
    uint16_t wbuf[256] = {0};
    guest_mem_read(&proc->mem, str_va, wbuf, len * 2);

    if (format & 0x0400 /* DT_CALCRECT */) {
        rc.right = rc.left + len * 8;
        rc.bottom = rc.top + 12;
        guest_mem_write(&proc->mem, rc_va, &rc, sizeof(rc));
        return 12;
    }

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 12;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 12;

    int x = rc.left;
    int y = rc.top;
    if (format & 0x0001 /* DT_CENTER */) {
        x = rc.left + ((rc.right - rc.left) - len * 8) / 2;
    } else if (format & 0x0002 /* DT_RIGHT */) {
        x = rc.right - len * 8;
    }
    if (format & 0x0004 /* DT_VCENTER */) {
        y = rc.top + ((rc.bottom - rc.top) - 8) / 2;
    }

    for (int i = 0; i < len; i++) {
        char ch = (char)(wbuf[i] & 0x7F);
        if (ch < 32 || ch > 126) ch = ' ';
        const uint8_t *glyph = GDI_FONT_8X8[ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= bm->bitmap.height) continue;
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                int px = x + (i * 8) + gx;
                if (px < 0 || px >= bm->bitmap.width) continue;
                if (row & (0x80 >> gx)) {
                    *(uint32_t *)(bm->bitmap.bits + (py * bm->bitmap.stride) + (px * 4)) = dc->text_color | 0xFF000000;
                } else if (dc->bk_mode == 2) {
                    *(uint32_t *)(bm->bitmap.bits + (py * bm->bitmap.stride) + (px * 4)) = dc->bk_color | 0xFF000000;
                }
            }
        }
    }
    return 12;
}

static uint32_t u32_DrawTextA(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    uint32_t str_va = args[1];
    int len = (int)args[2];
    uint32_t rc_va = args[3];
    uint32_t format = args[4];
    if (!str_va || !rc_va) return 0;
    const char *str = (const char *)guest_va_to_host(&proc->mem, str_va);
    if (!str) return 0;
    if (len < 0) len = (int)strlen(str);
    if (len > 255) len = 255;

    WIN_RECT rc = {0};
    guest_mem_read(&proc->mem, rc_va, &rc, sizeof(rc));
    if (format & 0x0400 /* DT_CALCRECT */) {
        rc.right = rc.left + len * 8;
        rc.bottom = rc.top + 12;
        guest_mem_write(&proc->mem, rc_va, &rc, sizeof(rc));
        return 12;
    }

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 12;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 12;

    int x = rc.left;
    int y = rc.top;
    if (format & 0x0001 /* DT_CENTER */) {
        x = rc.left + ((rc.right - rc.left) - len * 8) / 2;
    } else if (format & 0x0002 /* DT_RIGHT */) {
        x = rc.right - len * 8;
    }
    if (format & 0x0004 /* DT_VCENTER */) {
        y = rc.top + ((rc.bottom - rc.top) - 8) / 2;
    }

    for (int i = 0; i < len; i++) {
        char ch = str[i];
        if (ch < 32 || ch > 126) ch = ' ';
        const uint8_t *glyph = GDI_FONT_8X8[ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= bm->bitmap.height) continue;
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                int px = x + (i * 8) + gx;
                if (px < 0 || px >= bm->bitmap.width) continue;
                if (row & (0x80 >> gx)) {
                    *(uint32_t *)(bm->bitmap.bits + (py * bm->bitmap.stride) + (px * 4)) = dc->text_color | 0xFF000000;
                } else if (dc->bk_mode == 2) {
                    *(uint32_t *)(bm->bitmap.bits + (py * bm->bitmap.stride) + (px * 4)) = dc->bk_color | 0xFF000000;
                }
            }
        }
    }
    return 12;
}

void user32_init_apis(void) {
    win32_register_api("USER32.DLL", "RegisterClassA", 0, u32_RegisterClassA, 1);
    win32_register_api("USER32.DLL", "RegisterClassW", 0, u32_RegisterClassW, 1);
    win32_register_api("USER32.DLL", "CreateWindowExA", 0, u32_CreateWindowExA, 12);
    win32_register_api("USER32.DLL", "CreateWindowExW", 0, u32_CreateWindowExW, 12);
    win32_register_api("USER32.DLL", "ShowWindow", 0, u32_ShowWindow, 2);
    win32_register_api("USER32.DLL", "UpdateWindow", 0, u32_UpdateWindow, 1);
    win32_register_api("USER32.DLL", "DestroyWindow", 0, u32_DestroyWindow, 1);

    win32_register_api("USER32.DLL", "GetMessageA", 0, u32_GetMessageA, 4);
    win32_register_api("USER32.DLL", "PeekMessageA", 0, u32_PeekMessageA, 5);
    win32_register_api("USER32.DLL", "TranslateMessage", 0, u32_TranslateMessage, 1);
    win32_register_api("USER32.DLL", "DispatchMessageA", 0, u32_DispatchMessageA, 1);
    win32_register_api("USER32.DLL", "PostQuitMessage", 0, u32_PostQuitMessage, 1);
    win32_register_api("USER32.DLL", "PostMessageA", 0, u32_PostMessage, 4);
    win32_register_api("USER32.DLL", "PostMessageW", 0, u32_PostMessage, 4);
    win32_register_api("USER32.DLL", "DefWindowProcA", 0, u32_DefWindowProcA, 4);
    win32_register_api("USER32.DLL", "DefWindowProcW", 0, u32_DefWindowProcA, 4);

    win32_register_api("USER32.DLL", "BeginPaint", 0, u32_BeginPaint, 2);
    win32_register_api("USER32.DLL", "EndPaint", 0, u32_EndPaint, 2);
    win32_register_api("USER32.DLL", "GetDC", 0, u32_GetDC, 1);
    win32_register_api("USER32.DLL", "ReleaseDC", 0, u32_ReleaseDC, 2);
    win32_register_api("USER32.DLL", "MessageBoxA", 0, u32_MessageBoxA, 4);
    win32_register_api("USER32.DLL", "MessageBoxW", 0, u32_MessageBoxW, 4);

    win32_register_api("USER32.DLL", "LoadStringW", 0, u32_LoadStringW, 4);
    win32_register_api("USER32.DLL", "LoadIconW", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadIconA", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadCursorW", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadCursorA", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadMenuW", 0, u32_LoadMenuW, 2);
    win32_register_api("USER32.DLL", "LoadMenuA", 0, u32_LoadMenuA, 2);
    win32_register_api("USER32.DLL", "CallWindowProcW", 0, u32_CallWindowProcW, 5);
    win32_register_api("USER32.DLL", "CallWindowProcA", 0, u32_CallWindowProcW, 5);
    win32_register_api("USER32.DLL", "GetWindowLongA", 0, u32_GetWindowLongW, 2);
    win32_register_api("USER32.DLL", "SetWindowLongA", 0, u32_SetWindowLongW, 3);
    win32_register_api("USER32.DLL", "LoadAcceleratorsW", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadAcceleratorsA", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "TranslateAcceleratorA", 0, u32_TranslateAcceleratorW, 3);
    win32_register_api("USER32.DLL", "IsDialogMessageA", 0, u32_IsDialogMessageW, 2);
    win32_register_api("USER32.DLL", "GetWindowRect", 0, u32_GetWindowRect, 2);
    win32_register_api("USER32.DLL", "ScreenToClient", 0, u32_ScreenToClient, 2);
    win32_register_api("USER32.DLL", "MapDialogRect", 0, u32_MapDialogRect, 2);
    win32_register_api("USER32.DLL", "GetSysColorBrush", 0, u32_GetSysColorBrush, 1);
    win32_register_api("USER32.DLL", "CharUpperA", 0, u32_CharUpperA, 1);
    win32_register_api("USER32.DLL", "CharNextA", 0, u32_CharNextA, 1);
    win32_register_api("USER32.DLL", "CharPrevA", 0, u32_CharPrevA, 2);
    win32_register_api("USER32.DLL", "RegisterWindowMessageA", 0, u32_RegisterWindowMessageW, 1);
    win32_register_api("USER32.DLL", "TabbedTextOutA", 0, u32_DrawTextW, 8);
    win32_register_api("USER32.DLL", "ChildWindowFromPoint", 0, u32_SetFocus, 3);
    win32_register_api("USER32.DLL", "GetCursorPos", 0, u32_GetCursorPos, 1);
    win32_register_api("USER32.DLL", "GetLastActivePopup", 0, u32_SetFocus, 1);
    win32_register_api("USER32.DLL", "SetForegroundWindow", 0, u32_SetFocus, 1);
    win32_register_api("SHELL32.DLL", "DragAcceptFiles", 0, dummy_PlaySoundW, 2);
    win32_register_api("SHELL32.DLL", "DragFinish", 0, dummy_PlaySoundW, 1);
    win32_register_api("SHELL32.DLL", "DragQueryFileA", 0, u32_IsDialogMessageW, 4);
    win32_register_api("SHELL32.DLL", "ShellExecuteA", 0, dummy_PlaySoundW, 6);
    win32_register_api("SHELL32.DLL", "ShellAboutA", 0, dummy_ShellAboutW, 4);
    win32_register_api("USER32.DLL", "DrawEdge", 0, u32_DrawEdge, 4);
    win32_register_api("USER32.DLL", "DrawFrameControl", 0, u32_DrawFrameControl, 4);
    win32_register_api("USER32.DLL", "TrackPopupMenuEx", 0, u32_TrackPopupMenuEx, 6);
    win32_register_api("USER32.DLL", "GetClipboardData", 0, u32_GetClipboardData, 1);
    win32_register_api("USER32.DLL", "GetSystemMetrics", 0, u32_GetSystemMetrics, 1);
    win32_register_api("USER32.DLL", "GetDesktopWindow", 0, u32_GetDesktopWindow, 0);
    win32_register_api("USER32.DLL", "GetMenuItemRect", 0, u32_GetMenuItemRect, 4);
    win32_register_api("USER32.DLL", "MoveWindow", 0, u32_MoveWindow, 6);
    win32_register_api("USER32.DLL", "SetTimer", 0, u32_dummy_handle, 4);
    win32_register_api("USER32.DLL", "KillTimer", 0, u32_dummy_handle, 2);

    win32_register_api("USER32.DLL", "CheckMenuItem", 0, u32_CheckMenuItem, 3);
    win32_register_api("USER32.DLL", "SetMenu", 0, u32_SetMenu, 2);
    win32_register_api("USER32.DLL", "GetDlgItemInt", 0, u32_GetDlgItemInt, 4);
    win32_register_api("USER32.DLL", "ReleaseCapture", 0, u32_ReleaseCapture, 0);
    win32_register_api("USER32.DLL", "MapWindowPoints", 0, u32_MapWindowPoints, 4);
    win32_register_api("USER32.DLL", "SetCapture", 0, u32_SetCapture, 1);
    win32_register_api("USER32.DLL", "PtInRect", 0, u32_PtInRect, 3);
    win32_register_api("USER32.DLL", "WinHelpW", 0, u32_WinHelpW, 4);
    win32_register_api("USER32.DLL", "SetDlgItemInt", 0, u32_SetDlgItemInt, 4);
    win32_register_api("USER32.DLL", "EndDialog", 0, u32_EndDialog, 2);
    win32_register_api("USER32.DLL", "SetDlgItemTextW", 0, u32_SetDlgItemTextW, 3);
    win32_register_api("USER32.DLL", "wsprintfW", 0, u32_wsprintfW, 8);
    win32_register_api("USER32.DLL", "SendMessageW", 0, u32_SendMessageW, 4);
    win32_register_api("USER32.DLL", "GetDlgItem", 0, u32_GetDlgItem, 2);
    win32_register_api("USER32.DLL", "GetDlgItemTextW", 0, u32_GetDlgItemTextW, 4);
    win32_register_api("USER32.DLL", "InvalidateRect", 0, u32_InvalidateRect, 3);
    win32_register_api("USER32.DLL", "SetRect", 0, u32_SetRect, 5);
    win32_register_api("USER32.DLL", "DialogBoxParamW", 0, u32_DialogBoxParamW, 5);
    win32_register_api("USER32.DLL", "DialogBoxParamA", 0, u32_DialogBoxParamA, 5);
    win32_register_api("USER32.DLL", "CreateDialogParamW", 0, u32_CreateDialogParamW, 5);
    win32_register_api("USER32.DLL", "CreateDialogParamA", 0, u32_CreateDialogParamA, 5);
    win32_register_api("USER32.DLL", "CreateDialogIndirectParamW", 0, u32_CreateDialogIndirectParamW, 5);
    win32_register_api("USER32.DLL", "CreateDialogIndirectParamA", 0, u32_CreateDialogIndirectParamA, 5);
    win32_register_api("USER32.DLL", "TranslateAcceleratorW", 0, u32_TranslateAcceleratorW, 3);

    win32_register_api("USER32.DLL", "DefFrameProcW", 0, u32_DefFrameProcW, 5);
    win32_register_api("USER32.DLL", "DefMDIChildProcW", 0, u32_DefMDIChildProcW, 4);
    win32_register_api("USER32.DLL", "GetWindowLongW", 0, u32_GetWindowLongW, 2);
    win32_register_api("USER32.DLL", "SetWindowLongW", 0, u32_SetWindowLongW, 3);
    win32_register_api("USER32.DLL", "SetWindowPos", 0, u32_SetWindowPos, 7);
    win32_register_api("USER32.DLL", "SetWindowTextA", 0, u32_SetWindowTextA, 2);
    win32_register_api("USER32.DLL", "SetWindowTextW", 0, u32_SetWindowTextW, 2);
    win32_register_api("USER32.DLL", "MonitorFromWindow", 0, u32_MonitorFromWindow, 2);
    win32_register_api("USER32.DLL", "GetWindowTextA", 0, u32_GetWindowTextA, 3);
    win32_register_api("USER32.DLL", "GetClientRect", 0, u32_GetClientRect, 2);
    win32_register_api("USER32.DLL", "DrawIcon", 0, u32_DrawIcon, 4);
    win32_register_api("USER32.DLL", "CreateIcon", 0, u32_CreateIcon, 7);
    win32_register_api("USER32.DLL", "GetIconInfo", 0, u32_GetIconInfo, 2);
    win32_register_api("USER32.DLL", "ArrangeIconicWindows", 0, u32_ArrangeIconicWindows, 1);
    win32_register_api("USER32.DLL", "IsIconic", 0, u32_IsIconic, 1);
    win32_register_api("USER32.DLL", "IsZoomed", 0, u32_IsZoomed, 1);
    win32_register_api("USER32.DLL", "CloseWindow", 0, u32_CloseWindow, 1);
    win32_register_api("USER32.DLL", "OpenIcon", 0, u32_OpenIcon, 1);
    win32_register_api("USER32.DLL", "GetSubMenu", 0, u32_GetSubMenu, 2);
    win32_register_api("USER32.DLL", "EnableMenuItem", 0, u32_EnableMenuItem, 3);
    win32_register_api("USER32.DLL", "DestroyMenu", 0, u32_DestroyMenu, 1);
    win32_register_api("USER32.DLL", "CheckDlgButton", 0, u32_CheckDlgButton, 3);
    win32_register_api("USER32.DLL", "IsDlgButtonChecked", 0, u32_IsDlgButtonChecked, 2);
    win32_register_api("USER32.DLL", "CheckRadioButton", 0, u32_CheckRadioButton, 4);
    win32_register_api("USER32.DLL", "SendDlgItemMessageA", 0, u32_SendDlgItemMessageA, 5);
    win32_register_api("USER32.DLL", "SendDlgItemMessageW", 0, u32_SendDlgItemMessageW, 5);
    win32_register_api("USER32.DLL", "SetDlgItemTextA", 0, u32_SetDlgItemTextA, 3);
    win32_register_api("USER32.DLL", "GetDlgItemTextA", 0, u32_GetDlgItemTextA, 4);
    win32_register_api("USER32.DLL", "WinHelpA", 0, u32_WinHelpW, 4);
    win32_register_api("USER32.DLL", "LoadStringA", 0, u32_LoadStringA, 4);
    win32_register_api("USER32.DLL", "SendMessageA", 0, u32_SendMessageA, 4);
    win32_register_api("USER32.DLL", "wsprintfA", 0, u32_wsprintfW, 8);

    win32_register_api("USER32.DLL", "AdjustWindowRect", 0, u32_AdjustWindowRect, 3);
    win32_register_api("USER32.DLL", "GetSysColor", 0, u32_GetSysColor, 1);
    win32_register_api("USER32.DLL", "CopyRect", 0, u32_CopyRect, 2);
    win32_register_api("USER32.DLL", "IntersectRect", 0, u32_IntersectRect, 3);
    win32_register_api("USER32.DLL", "InflateRect", 0, u32_InflateRect, 3);
    win32_register_api("USER32.DLL", "InvertRect", 0, u32_InvertRect, 2);
    win32_register_api("USER32.DLL", "FrameRect", 0, u32_FrameRect, 3);
    win32_register_api("USER32.DLL", "ClientToScreen", 0, u32_ClientToScreen, 2);
    win32_register_api("USER32.DLL", "GetKeyState", 0, u32_GetKeyState, 1);
    win32_register_api("USER32.DLL", "SetFocus", 0, u32_SetFocus, 1);
    win32_register_api("USER32.DLL", "EnableWindow", 0, u32_EnableWindow, 2);
    win32_register_api("USER32.DLL", "ShowCursor", 0, u32_ShowCursor, 1);
    win32_register_api("USER32.DLL", "GetCapture", 0, u32_GetCapture, 0);
    win32_register_api("USER32.DLL", "GetMenu", 0, u32_GetMenu, 1);
    win32_register_api("USER32.DLL", "RegisterClassExW", 0, u32_RegisterClassExW, 1);
    win32_register_api("USER32.DLL", "RegisterClassExA", 0, u32_RegisterClassExA, 1);
    win32_register_api("USER32.DLL", "LoadImageW", 0, u32_LoadImageW, 6);
    win32_register_api("USER32.DLL", "RegisterWindowMessageW", 0, u32_RegisterWindowMessageW, 1);
    win32_register_api("USER32.DLL", "SetCursorPos", 0, u32_SetCursorPos, 2);
    win32_register_api("USER32.DLL", "MsgWaitForMultipleObjects", 0, u32_MsgWaitForMultipleObjects, 5);
    win32_register_api("USER32.DLL", "DrawTextW", 0, u32_DrawTextW, 5);
    win32_register_api("USER32.DLL", "DrawTextA", 0, u32_DrawTextA, 5);

    win32_register_api("USER32.DLL", "CharUpperW", 0, u32_CharUpperW, 1);
    win32_register_api("USER32.DLL", "CharNextW", 0, u32_CharNextW, 1);
    win32_register_api("USER32.DLL", "SetCursor", 0, u32_SetCursor, 1);
    win32_register_api("USER32.DLL", "GetFocus", 0, u32_GetFocus, 0);
    win32_register_api("USER32.DLL", "GetForegroundWindow", 0, u32_GetForegroundWindow, 0);
    win32_register_api("USER32.DLL", "IsWindow", 0, u32_IsWindow, 1);
    win32_register_api("USER32.DLL", "GetWindowPlacement", 0, u32_GetWindowPlacement, 2);
    win32_register_api("USER32.DLL", "SetWindowPlacement", 0, u32_SetWindowPlacement, 2);
    win32_register_api("USER32.DLL", "GetWindowTextW", 0, u32_GetWindowTextW, 3);
    win32_register_api("USER32.DLL", "GetWindowTextLengthW", 0, u32_GetWindowTextLengthW, 1);
    win32_register_api("USER32.DLL", "GetSystemMenu", 0, u32_GetSystemMenu, 2);
    win32_register_api("USER32.DLL", "SetWinEventHook", 0, u32_SetWinEventHook, 7);
    win32_register_api("USER32.DLL", "UnhookWinEvent", 0, u32_UnhookWinEvent, 1);
    win32_register_api("USER32.DLL", "IsDialogMessageW", 0, u32_IsDialogMessageW, 2);
    win32_register_api("USER32.DLL", "DispatchMessageW", 0, u32_DispatchMessageW, 1);
    win32_register_api("USER32.DLL", "GetMessageW", 0, u32_GetMessageW, 4);
    win32_register_api("USER32.DLL", "PeekMessageW", 0, u32_PeekMessageW, 5);
    win32_register_api("USER32.DLL", "RedrawWindow", 0, u32_RedrawWindow, 4);
    win32_register_api("USER32.DLL", "SetScrollPos", 0, u32_SetScrollPos, 4);
    win32_register_api("USER32.DLL", "GetDlgCtrlID", 0, u32_GetDlgCtrlID, 1);
    win32_register_api("USER32.DLL", "GetKeyboardLayout", 0, u32_GetKeyboardLayout, 1);
    win32_register_api("USER32.DLL", "TrackMouseEvent", 0, u32_TrackMouseEvent, 1);
    win32_register_api("USER32.DLL", "DestroyIcon", 0, u32_DestroyIcon, 1);
    win32_register_api("USER32.DLL", "MessageBeep", 0, u32_MessageBeep, 1);
    win32_register_api("USER32.DLL", "GetPropW", 0, u32_GetPropW, 2);
    win32_register_api("USER32.DLL", "SetPropW", 0, u32_SetPropW, 3);
    win32_register_api("USER32.DLL", "RemovePropW", 0, u32_RemovePropW, 2);
    win32_register_api("USER32.DLL", "OpenClipboard", 0, u32_OpenClipboard, 1);
    win32_register_api("USER32.DLL", "IsClipboardFormatAvailable", 0, u32_IsClipboardFormatAvailable, 1);
    win32_register_api("USER32.DLL", "CloseClipboard", 0, u32_CloseClipboard, 0);
    win32_register_api("USER32.DLL", "DrawFocusRect", 0, u32_DrawFocusRect, 2);
    win32_register_api("USER32.DLL", "DrawTextExW", 0, u32_DrawTextExW, 6);
    win32_register_api("USER32.DLL", "NotifyWinEvent", 0, u32_NotifyWinEvent, 4);
    win32_register_api("USER32.DLL", "FillRect", 0, u32_FillRect, 3);
    win32_register_api("USER32.DLL", "SetActiveWindow", 0, u32_SetFocus, 1);

    win32_register_api("SHELL32.DLL", "ShellAboutW", 0, dummy_ShellAboutW, 4);
    win32_register_api("WINMM.DLL", "PlaySoundW", 0, dummy_PlaySoundW, 3);
    win32_register_api("COMCTL32.DLL", "InitCommonControlsEx", 0, dummy_InitCommonControlsEx, 1);
    win32_register_api("COMCTL32.DLL", "CreateStatusWindowW", 0, u32_CreateStatusWindowW, 4);
}
