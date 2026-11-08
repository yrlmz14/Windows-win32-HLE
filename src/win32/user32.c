#include "user32.h"
#include "gdi32.h"
#include "win32_dispatch.h"
#include "../platform/kindle_touch.h"
#include "../platform/eink_render.h"
#include "../platform/vkeyboard.h"
#include <strings.h>

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

void user32_init_apis(void);

void user32_init(void) {
    memset(s_classes, 0, sizeof(s_classes));
    s_class_count = 0;
    memset(s_windows, 0, sizeof(s_windows));
    s_msg_head = s_msg_tail = 0;
    s_active_hwnd = 0;
    s_capture_hwnd = 0;
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

void user_set_active_window(H_HWND hwnd) {
    s_active_hwnd = hwnd;
}

bool user_post_message(H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int next = (s_msg_tail + 1) % MSG_QUEUE_CAP;
    if (next == s_msg_head) {
        LOGW("Message queue full, dropping message 0x%04X", msg);
        return false;
    }
    s_msg_queue[s_msg_tail].hwnd = hwnd;
    s_msg_queue[s_msg_tail].message = msg;
    s_msg_queue[s_msg_tail].wParam = wParam;
    s_msg_queue[s_msg_tail].lParam = lParam;
    s_msg_queue[s_msg_tail].pt = s_cursor_pos;
    s_msg_tail = next;
    return true;
}

/* Call Window Procedure in guest x86 space */
static LRESULT call_wndproc(win32_process_t *proc, GUEST_PTR wndproc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!proc || !wndproc) return 0;

    LOGI("call_wndproc: HWND 0x%08X, msg 0x%04X (wP=0x%08X, lP=0x%08X), wndproc=0x%08X",
         hwnd, msg, (uint32_t)wParam, (uint32_t)lParam, wndproc);

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
    } else {
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

    LOGI("RegisterClassA: %s (WndProc=0x%08X)", name, wc->lpfnWndProc);
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
#define MAX_ICONS 64

typedef struct {
    bool in_use;
    int width;
    int height;
    uint32_t pixels[32 * 32]; /* 32x32 ARGB */
} win32_icon_entry_t;

static win32_icon_entry_t s_icons[MAX_ICONS];

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

#define MAX_MENUS 32
#define MAX_MENU_ITEMS 16

typedef struct {
    char title[32];
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

static uint8_t *allocate_window_framebuffer(int w, int h, uint32_t bg_color) {
    if (w <= 0 || h <= 0) return NULL;
    uint32_t *fb = (uint32_t *)malloc(w * h * sizeof(uint32_t));
    if (!fb) return NULL;
    for (int i = 0; i < w * h; i++) {
        fb[i] = bg_color;
    }
    return (uint8_t *)fb;
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
    bool is_iconic = is_child && has_caption && (bw <= 96 && bh <= 80);

    /* 1. Base window surface / background filling */
    if (strcmp(w->class_name, "MDICLIENT") == 0) {
        /* Standard Windows MDI Workspace */
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFF808080);
        return;
    }

    if (is_iconic) {
        /* Iconic child window (e.g. desktop/group program icon):
         * Icon pixels are drawn by DrawIcon at (bw/2 - 16, 2).
         * Caption label is drawn centered below the 32x32 icon (y >= 36). */
        const char *label = w->title[0] ? w->title : "";
        if (label[0]) {
            int text_len = (int)strlen(label);
            int text_w = text_len * 8 + 6;
            int text_x = (bw - text_w) / 2;
            if (text_x < 0) text_x = 0;
            if (text_w > bw) text_w = bw;
            int text_y = 36;
            if (is_active) {
                fill_rect_argb(fb, stride, bw, bh, text_x, text_y, text_w, 14, 0xFF000080);
                draw_text_argb(fb, stride, bw, bh, text_x + 3, text_y + 3, label, 0xFFFFFFFF);
            } else {
                draw_text_argb(fb, stride, bw, bh, text_x + 3, text_y + 3, label, 0xFF000000);
            }
        }
        return;
    }

    if (is_child && !has_caption) {
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, 0xFFC0C0C0);
    } else {
        uint32_t bkg_color = (is_child && has_caption) ? 0xFFFFFFFF : 0xFFC0C0C0;
        fill_rect_argb(fb, stride, bw, bh, 0, 0, bw, bh, bkg_color);
    }

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

    /* Lookup class wndproc */
    for (int i = 0; i < s_class_count; i++) {
        if (strcasecmp(s_classes[i].class_name, win->class_name) == 0) {
            win->wndproc = s_classes[i].wndproc;
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
    win->framebuffer = allocate_window_framebuffer(w, h, 0xFFC0C0C0);
    paint_window_decorations(win);

    /* Point window DC's default bitmap to window framebuffer */
    gdi_dc_t *dc = gdi_get_dc(win->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        bm_obj->bitmap.width = w;
        bm_obj->bitmap.height = h;
        bm_obj->bitmap.bpp = 32;
        bm_obj->bitmap.stride = win->fb_stride;
        bm_obj->bitmap.bits = win->framebuffer;
        dc->selected_bitmap = (H_HBITMAP)hbm;
    }

    s_active_hwnd = hwnd;
    LOGI("CreateWindowExA: '%s' (%s) -> HWND 0x%08X [%d,%d %dx%d]",
         win->title, win->class_name, hwnd, x, y, w, h);

    /* Send WM_CREATE */
    user_send_message(proc, hwnd, WM_CREATE, 0, 0, NULL);

    return (uint32_t)hwnd;
}

static uint32_t u32_ShowWindow(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    int cmd = (int)args[1];
    win32_window_t *w = user_get_window(hwnd);
    if (w) {
        w->visible = (cmd != SW_HIDE);
        w->needs_redraw = true;
        s_active_hwnd = hwnd;
        user_post_message(hwnd, WM_PAINT, 0, 0);
        return 1;
    }
    return 0;
}

static uint32_t u32_UpdateWindow(win32_process_t *proc, uint32_t *args) {
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w && w->visible) {
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

    /* No messages queued: return WM_NULL and yield CPU */
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
    memset(out_msg, 0, sizeof(WIN_MSG));
    out_msg->message = 0; /* WM_NULL */
    return 1;
}

static uint32_t u32_PeekMessageA(win32_process_t *proc, uint32_t *args) {
    uint32_t msg_va = args[0];
    /* uint32_t hwnd = args[1]; */
    uint32_t remove = args[4];

    WIN_MSG *out_msg = (WIN_MSG *)guest_va_to_host(&proc->mem, msg_va);
    if (!out_msg) return 0;

    /* If queue is empty, poll hardware inputs so modal loops (smiley button or drag) get release/move events! */
    if (s_msg_head == s_msg_tail) {
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
    }

    if (s_msg_head != s_msg_tail) {
        *out_msg = s_msg_queue[s_msg_head];
        if (remove & 1) { /* PM_REMOVE */
            s_msg_head = (s_msg_head + 1) % MSG_QUEUE_CAP;
        }
        return 1;
    }

    /* Check if any window needs WM_PAINT */
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

    /* Yield CPU slightly in modal PeekMessage spins */
    sys_sleep_ms(2);
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
    if (msg == 0x0201 /* WM_LBUTTONDOWN */) {
        user_post_message(hwnd, 0x00A1 /* WM_NCLBUTTONDOWN */, 2 /* HTCAPTION */, lParam);
        return 0;
    }
    if (msg == WM_PAINT || msg == 0x0085 /* WM_NCPAINT */ || msg == 0x0014 /* WM_ERASEBKGND */) {
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
    return 1;
}

static uint32_t u32_GetDC(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HWND hwnd = (H_HWND)args[0];
    win32_window_t *w = user_get_window(hwnd);
    if (w) return (uint32_t)w->hdc;
    return 0;
}

static uint32_t u32_ReleaseDC(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t u32_MessageBoxA(win32_process_t *proc, uint32_t *args) {
    const char *text = (const char *)guest_va_to_host(&proc->mem, args[1]);
    const char *caption = (const char *)guest_va_to_host(&proc->mem, args[2]);
    LOGI(">>> MessageBoxA: [%s] %s <<<", caption ? caption : "Message", text ? text : "");
    return 1; /* IDOK */
}

static void get_window_screen_pos(win32_window_t *w, int *out_x, int *out_y) {
    int sx = w->x;
    int sy = w->y;
    H_HWND cur_parent = w->parent;
    while (cur_parent != 0) {
        win32_window_t *pw = user_get_window(cur_parent);
        if (!pw) break;
        sx += pw->x;
        sy += pw->y;
        cur_parent = pw->parent;
    }
    *out_x = sx;
    *out_y = sy;
}

/* Touch & Mouse Event Injection */
void user_inject_mouse_down(int screen_x, int screen_y, bool right_button) {
    s_cursor_pos.x = screen_x;
    s_cursor_pos.y = screen_y;

    /* Find top-most window hit (children before parents) */
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        if (!s_windows[i].in_use || !s_windows[i].visible) continue;
        win32_window_t *w = &s_windows[i];
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
            bool is_iconic = is_child && has_caption && (w->width <= 96 && w->height <= 80);

            if (is_iconic) {
                UINT msg = right_button ? 0x00A4 /* WM_NCRBUTTONDOWN */ : 0x00A1 /* WM_NCLBUTTONDOWN */;
                user_post_message(w->hwnd, msg, 2 /* HTCAPTION */, lp);
            } else if (has_caption && rel_y < 22) {
                UINT msg = right_button ? 0x00A4 /* WM_NCRBUTTONDOWN */ : 0x00A1 /* WM_NCLBUTTONDOWN */;
                user_post_message(w->hwnd, msg, 2 /* HTCAPTION */, lp);
            } else {
                UINT msg = right_button ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
                user_post_message(w->hwnd, msg, 1, lp);
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
        LPARAM lp = (uint32_t)(rel_y << 16) | (uint16_t)rel_x;
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
            bool has_caption = (w->style & WS_CAPTION) == WS_CAPTION;
            bool is_iconic = is_child && has_caption && (w->width <= 96 && w->height <= 80);

            if (is_iconic || (has_caption && rel_y < 22)) {
                user_post_message(w->hwnd, 0x00A3 /* WM_NCLBUTTONDBLCLK */, 2 /* HTCAPTION */, lp);
            } else {
                user_post_message(w->hwnd, 0x0203 /* WM_LBUTTONDBLCLK */, 1, lp);
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
        LPARAM lp = (uint32_t)(rel_y << 16) | (uint16_t)rel_x;
        user_post_message(w->hwnd, WM_MOUSEMOVE, 0, lp);
    }
}

void user_inject_key(int vkey, char ch, bool down) {
    if (s_active_hwnd) {
        UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
        user_post_message(s_active_hwnd, msg, vkey, 0);
        if (down && ch >= 32 && ch <= 126) {
            user_post_message(s_active_hwnd, WM_CHAR, ch, 0);
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
                /* Skip uninitialized / transparent pixels (alpha == 0) */
                uint8_t a = (px >> 24) & 0xFF;
                if (a == 0) continue;

                uint8_t r = (px >> 16) & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = px & 0xFF;
                uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);

                dest_canvas[(dy * canvas_stride) + dx] = gray;
            }
        }
    }
}

static void wchar_to_ascii(const uint16_t *src, char *dst, size_t max_len) {
    if (!src || !dst || max_len == 0) return;
    size_t i = 0;
    while (src[i] != 0 && i < max_len - 1) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
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

    uint32_t wndproc = 0, name_va = 0, icon = 0, cursor = 0, brush = 0;
    guest_mem_read(&proc->mem, wc_va + 4, &wndproc, 4);
    guest_mem_read(&proc->mem, wc_va + 20, &icon, 4);
    guest_mem_read(&proc->mem, wc_va + 24, &cursor, 4);
    guest_mem_read(&proc->mem, wc_va + 28, &brush, 4);
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

    LOGI("RegisterClassW: %s (WndProc=0x%08X)", ascii_name, wndproc);
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
    if (strcmp(class_name, "MDICLIENT") == 0) {
        x = 4;
        y = 44;
        if (parent) {
            win32_window_t *pw = user_get_window(parent);
            if (pw) {
                w = pw->width - 8;
                h = pw->height - 48;
            }
        }
    } else {
        bool is_child = (parent != 0);
        bool is_iconic = is_child && ((style & WS_CAPTION) == WS_CAPTION) && (w <= 0 || w == (int)0x80000000 || w <= 96);
        if (is_iconic) {
            if (w <= 0 || w == (int)0x80000000) w = 76;
            if (h <= 0 || h == (int)0x80000000) h = 60;
        } else if (is_child) {
            if (w <= 0 || w == (int)0x80000000) w = 400;
            if (h <= 0 || h == (int)0x80000000) h = 260;
        } else {
            if (x == (int)0x80000000 || x < 0) x = (DEFAULT_SCREEN_W > 640) ? (DEFAULT_SCREEN_W - 640) / 2 : 40;
            if (y == (int)0x80000000 || y < 0) y = (DEFAULT_SCREEN_H > 480) ? (DEFAULT_SCREEN_H - 480) / 2 : 40;
            if (w <= 0 || w == (int)0x80000000) w = 640;
            if (h <= 0 || h == (int)0x80000000) h = 480;
        }
    }

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
            break;
        }
    }

    win->hdc = gdi_create_window_dc(hwnd);
    win->fb_stride = w * 4;
    win->framebuffer = allocate_window_framebuffer(w, h, 0xFFC0C0C0);
    paint_window_decorations(win);

    gdi_dc_t *dc = gdi_get_dc(win->hdc);
    if (dc) {
        H_HANDLE hbm = gdi_alloc_obj(GDI_OBJ_BITMAP);
        gdi_object_t *bm_obj = gdi_get_obj(hbm);
        if (bm_obj) {
            bm_obj->bitmap.width = w;
            bm_obj->bitmap.height = h;
            bm_obj->bitmap.bpp = 32;
            bm_obj->bitmap.stride = win->fb_stride;
            bm_obj->bitmap.bits = win->framebuffer;
            dc->selected_bitmap = (H_HBITMAP)hbm;
        }
    }

    s_active_hwnd = hwnd;
    LOGI("CreateWindowExW: '%s' (%s) -> HWND 0x%08X [%d,%d %dx%d]",
         win->title, win->class_name, hwnd, x, y, w, h);

    user_send_message(proc, hwnd, WM_CREATE, 0, 0, NULL);
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

    win32_window_t *win = user_get_window(hwnd);
    if (win) {
        if (x > 0) win->x = x;
        if (y > 0) win->y = y;
        if (w > 0 && h > 0 && (w != win->width || h != win->height)) {
            win->width = w;
            win->height = h;
            win->fb_stride = w * 4;
            if (win->framebuffer) free(win->framebuffer);
            win->framebuffer = allocate_window_framebuffer(w, h, 0xFFC0C0C0);

            gdi_dc_t *dc = gdi_get_dc(win->hdc);
            if (dc) {
                gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
                if (bm) {
                    bm->bitmap.width = w;
                    bm->bitmap.height = h;
                    bm->bitmap.stride = win->fb_stride;
                    bm->bitmap.bits = win->framebuffer;
                }
            }
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
    (void)proc; (void)args;
    return 0;
}

static uint32_t u32_PtInRect(win32_process_t *proc, uint32_t *args) {
    uint32_t lprc_va = args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    WIN_RECT rc;
    if (!guest_mem_read(&proc->mem, lprc_va, &rc, sizeof(rc))) return 0;
    if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom) return 1;
    return 0;
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

static uint32_t u32_SetDlgItemTextW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t handle_send_message(win32_process_t *proc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool unicode) {
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
    return 0x00004200 + args[1];
}

static uint32_t u32_GetDlgItemTextW(win32_process_t *proc, uint32_t *args) {
    uint32_t buf_va = args[2];
    int max_len = (int)args[3];
    if (buf_va && max_len > 0) {
        uint16_t zero = 0;
        guest_mem_write(&proc->mem, buf_va, &zero, sizeof(zero));
    }
    return 0;
}

static uint32_t u32_DialogBoxParamW(win32_process_t *proc, uint32_t *args) {
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
                w->framebuffer = allocate_window_framebuffer(cx, cy, 0xFFC0C0C0);
                paint_window_decorations(w);
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
        int32_t rect[4] = { 0, 0, w->width, w->height };
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
static uint32_t u32_LoadMenuW(win32_process_t *proc, uint32_t *args) {
    uint32_t menu_id = args[1];
    uint32_t data_sz = 0;
    uint32_t menu_va = 0;

    if (menu_id <= 0xFFFF && menu_id != 0) {
        menu_va = pe_find_resource_data(proc, 4 /* RT_MENU */, menu_id, &data_sz);
    }
    if (!menu_va) {
        menu_va = pe_find_resource_data(proc, 4, 265, &data_sz);
        if (!menu_va) menu_va = pe_find_resource_data(proc, 4, 1, &data_sz);
    }

    int root_slot = -1;
    for (int i = 1; i < MAX_MENUS; i++) {
        if (!s_menus[i].in_use) { root_slot = i; break; }
    }
    if (root_slot == -1) return 0x5001;

    win32_menu_t *root_menu = &s_menus[root_slot];
    memset(root_menu, 0, sizeof(win32_menu_t));
    root_menu->in_use = true;
    root_menu->hmenu = (H_HMENU)(0x5000 + root_slot);

    if (!menu_va || data_sz < 4) {
        return (uint32_t)root_menu->hmenu;
    }

    uint8_t *menu_data = guest_va_to_host(&proc->mem, menu_va);
    if (!menu_data) return (uint32_t)root_menu->hmenu;

    uint16_t off = *(uint16_t *)(menu_data + 2);
    uint8_t *p = menu_data + 4 + off;
    uint8_t *end = menu_data + data_sz;

    while (p < end && root_menu->item_count < MAX_MENU_ITEMS) {
        uint16_t flags = *(uint16_t *)p;
        p += 2;
        uint16_t item_id = 0;
        if (!(flags & 0x10)) {
            item_id = *(uint16_t *)p;
            p += 2;
        }

        char title[32];
        int ti = 0;
        while (p + 2 <= end) {
            uint16_t ch = *(uint16_t *)p;
            p += 2;
            if (ch == 0) break;
            if (ti < 31 && ch != '&') title[ti++] = (char)ch;
        }
        title[ti] = '\0';

        win32_menu_item_t *item = &root_menu->items[root_menu->item_count++];
        strncpy(item->title, title, sizeof(item->title) - 1);
        item->flags = flags;
        item->id = item_id;

        if (flags & 0x10) {
            int sub_slot = -1;
            for (int s = 1; s < MAX_MENUS; s++) {
                if (!s_menus[s].in_use && s != root_slot) { sub_slot = s; break; }
            }
            if (sub_slot != -1) {
                s_menus[sub_slot].in_use = true;
                s_menus[sub_slot].hmenu = (H_HMENU)(0x5000 + sub_slot);
                item->hsub = s_menus[sub_slot].hmenu;
            }

            int depth = 1;
            while (depth > 0 && p < end) {
                uint16_t cflags = *(uint16_t *)p;
                p += 2;
                if (!(cflags & 0x10)) {
                    p += 2;
                }
                while (p + 2 <= end) {
                    uint16_t ch = *(uint16_t *)p;
                    p += 2;
                    if (ch == 0) break;
                }
                if (cflags & 0x10) depth++;
                if (cflags & 0x80) depth--;
            }
        }

        if (flags & 0x80) break;
    }

    LOGI("u32_LoadMenuW: Loaded menu 0x%04X with %d top-level items",
         root_menu->hmenu, root_menu->item_count);
    return (uint32_t)root_menu->hmenu;
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
    (void)proc; (void)args;
    return 0;
}
static uint32_t u32_SetDlgItemTextA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}
static uint32_t u32_GetDlgItemTextA(win32_process_t *proc, uint32_t *args) {
    (void)proc;
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

void user32_init_apis(void) {
    win32_register_api("USER32.DLL", "RegisterClassA", 0, u32_RegisterClassA, 1);
    win32_register_api("USER32.DLL", "RegisterClassW", 0, u32_RegisterClassW, 1);
    win32_register_api("USER32.DLL", "CreateWindowExA", 0, u32_CreateWindowExA, 12);
    win32_register_api("USER32.DLL", "CreateWindowExW", 0, u32_CreateWindowExW, 12);
    win32_register_api("USER32.DLL", "ShowWindow", 0, u32_ShowWindow, 2);
    win32_register_api("USER32.DLL", "UpdateWindow", 0, u32_UpdateWindow, 1);
    win32_register_api("USER32.DLL", "DestroyWindow", 0, u32_DestroyWindow, 1);

    win32_register_api("USER32.DLL", "GetMessageA", 0, u32_GetMessageA, 4);
    win32_register_api("USER32.DLL", "GetMessageW", 0, u32_GetMessageA, 4);
    win32_register_api("USER32.DLL", "PeekMessageA", 0, u32_PeekMessageA, 5);
    win32_register_api("USER32.DLL", "PeekMessageW", 0, u32_PeekMessageA, 5);
    win32_register_api("USER32.DLL", "TranslateMessage", 0, u32_TranslateMessage, 1);
    win32_register_api("USER32.DLL", "DispatchMessageA", 0, u32_DispatchMessageA, 1);
    win32_register_api("USER32.DLL", "DispatchMessageW", 0, u32_DispatchMessageA, 1);
    win32_register_api("USER32.DLL", "PostQuitMessage", 0, u32_PostQuitMessage, 1);
    win32_register_api("USER32.DLL", "PostMessageW", 0, u32_PostQuitMessage, 4);
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
    win32_register_api("USER32.DLL", "LoadCursorW", 0, u32_dummy_handle, 2);
    win32_register_api("USER32.DLL", "LoadMenuW", 0, u32_LoadMenuW, 2);
    win32_register_api("USER32.DLL", "LoadMenuA", 0, u32_LoadMenuW, 2);
    win32_register_api("USER32.DLL", "LoadAcceleratorsW", 0, u32_dummy_handle, 2);
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
    win32_register_api("USER32.DLL", "TranslateAcceleratorW", 0, u32_TranslateAcceleratorW, 3);

    win32_register_api("USER32.DLL", "DefFrameProcW", 0, u32_DefFrameProcW, 5);
    win32_register_api("USER32.DLL", "DefMDIChildProcW", 0, u32_DefMDIChildProcW, 4);
    win32_register_api("USER32.DLL", "GetWindowLongW", 0, u32_GetWindowLongW, 2);
    win32_register_api("USER32.DLL", "SetWindowLongW", 0, u32_SetWindowLongW, 3);
    win32_register_api("USER32.DLL", "SetWindowPos", 0, u32_SetWindowPos, 7);
    win32_register_api("USER32.DLL", "SetWindowTextA", 0, u32_SetWindowTextA, 2);
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
    win32_register_api("USER32.DLL", "SendDlgItemMessageW", 0, u32_SendDlgItemMessageA, 5);
    win32_register_api("USER32.DLL", "SetDlgItemTextA", 0, u32_SetDlgItemTextA, 3);
    win32_register_api("USER32.DLL", "GetDlgItemTextA", 0, u32_GetDlgItemTextA, 4);
    win32_register_api("USER32.DLL", "WinHelpA", 0, u32_WinHelpW, 4);
    win32_register_api("USER32.DLL", "LoadStringA", 0, u32_LoadStringA, 4);
    win32_register_api("USER32.DLL", "SendMessageA", 0, u32_SendMessageA, 4);
    win32_register_api("USER32.DLL", "wsprintfA", 0, u32_wsprintfW, 8);

    win32_register_api("SHELL32.DLL", "ShellAboutW", 0, dummy_ShellAboutW, 4);
    win32_register_api("WINMM.DLL", "PlaySoundW", 0, dummy_PlaySoundW, 3);
    win32_register_api("COMCTL32.DLL", "InitCommonControlsEx", 0, dummy_InitCommonControlsEx, 1);
}
