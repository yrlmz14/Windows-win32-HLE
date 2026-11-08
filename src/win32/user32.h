#ifndef USER32_H
#define USER32_H

#include "../common.h"
#include "../pe_loader.h"
#include "win32_types.h"

#define MAX_WINDOWS     32
#define MAX_CLASSES     32
#define MSG_QUEUE_CAP   64

typedef struct {
    char class_name[64];
    GUEST_PTR wndproc;
    H_HICON   icon;
    H_HCURSOR cursor;
    H_HBRUSH  background_brush;
} win32_class_t;

typedef struct {
    bool in_use;
    H_HWND hwnd;
    H_HWND parent;
    char title[128];
    char class_name[64];
    GUEST_PTR wndproc;
    DWORD style;
    DWORD ex_style;
    int x, y, width, height;
    int client_x, client_y, client_w, client_h;
    bool visible;
    bool enabled;
    H_HMENU menu;
    H_HDC hdc;
    uint8_t *framebuffer; /* 32-bit ARGB surface for client/window area */
    int fb_stride;
    bool needs_redraw;
    uint32_t user_data;
    uint32_t extra_bytes[16];
    bool minimized;
    bool maximized;
} win32_window_t;

void user32_init(void);

/* Window & Message Loop Helpers */
win32_window_t *user_get_window(H_HWND hwnd);
H_HWND user_get_desktop_window(void);
H_HWND user_get_active_window(void);
void user_set_active_window(H_HWND hwnd);

/* Icon Management */
H_HICON user_extract_icon(const char *exe_path, int icon_index);

bool user_post_message(H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool user_send_message(win32_process_t *proc, H_HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result);

/* Window Manager Compositor for E-Ink */
void user_composite_windows(uint8_t *dest_canvas, int canvas_w, int canvas_h, int canvas_stride);

/* Touch/Mouse event injection */
void user_inject_mouse_down(int screen_x, int screen_y, bool right_button);
void user_inject_mouse_up(int screen_x, int screen_y, bool right_button);
void user_inject_mouse_dblclk(int screen_x, int screen_y);
void user_inject_mouse_move(int screen_x, int screen_y);
void user_inject_key(int vkey, char ch, bool down);

#endif /* USER32_H */
