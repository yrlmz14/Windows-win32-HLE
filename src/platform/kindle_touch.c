#include "kindle_touch.h"
#include "eink_render.h"
#include "vkeyboard.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* Forward declarations for user message injection */
void user_inject_mouse_down(int x, int y, bool is_right);
void user_inject_mouse_up(int x, int y, bool is_right);
void user_inject_mouse_dblclk(int x, int y);
void user_inject_mouse_move(int x, int y);
void user_inject_key(int vk, char ch, bool is_down);

static bool s_touch_dirty = false;
static bool s_right_click_toggle = false;

bool touch_take_dirty(void) {
    bool d = s_touch_dirty;
    s_touch_dirty = false;
    return d;
}

bool touch_is_right_click_mode(void) { return s_right_click_toggle; }
void touch_set_right_click_mode(bool on) { s_right_click_toggle = on; }

#ifdef _WIN32
#include <windows.h>

LRESULT CALLBACK SimWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int w = DEFAULT_SCREEN_W;
    int h = DEFAULT_SCREEN_H;
    int stride;
    eink_get_canvas(&w, &h, &stride);

    RECT rc;
    GetClientRect(hwnd, &rc);
    float sx = (rc.right > 0) ? ((float)w / (float)rc.right) : 1.0f;
    float sy = (rc.bottom > 0) ? ((float)h / (float)rc.bottom) : 1.0f;

    int mx = (int)(LOWORD(lParam) * sx);
    int my = (int)(HIWORD(lParam) * sy);

    switch (msg) {
        case WM_LBUTTONDOWN:
            s_touch_dirty = true;
            if (vkeyboard_is_visible() && vkeyboard_handle_tap(mx, my)) return 0;
            user_inject_mouse_down(mx, my, s_right_click_toggle);
            s_right_click_toggle = false;
            return 0;
        case WM_LBUTTONDBLCLK:
            s_touch_dirty = true;
            user_inject_mouse_dblclk(mx, my);
            return 0;
        case WM_LBUTTONUP:
            s_touch_dirty = true;
            user_inject_mouse_up(mx, my, false);
            return 0;
        case WM_RBUTTONDOWN:
            s_touch_dirty = true;
            user_inject_mouse_down(mx, my, true);
            return 0;
        case WM_RBUTTONUP:
            s_touch_dirty = true;
            user_inject_mouse_up(mx, my, true);
            return 0;
        case WM_MOUSEMOVE:
            user_inject_mouse_move(mx, my);
            return 0;
        case WM_KEYDOWN:
            s_touch_dirty = true;
            user_inject_key((int)wParam, 0, true);
            return 0;
        case WM_KEYUP:
            s_touch_dirty = true;
            user_inject_key((int)wParam, 0, false);
            return 0;
        case WM_CHAR:
            s_touch_dirty = true;
            user_inject_key(0, (char)wParam, true);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            (void)hdc;
            eink_present(EINK_REFRESH_FAST);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            exit(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

int touch_init(void) { return 0; }
void touch_cleanup(void) {}
void touch_poll(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

#else
#include <linux/input.h>
#include <sys/ioctl.h>

static int s_touch_fds[8];
static int s_num_touch_fds = 0;
static int s_max_x = DEFAULT_SCREEN_W;
static int s_max_y = DEFAULT_SCREEN_H;
static int s_raw_x = 0;
static int s_raw_y = 0;
static int s_cur_x = 0;
static int s_cur_y = 0;
static bool s_is_down = false;
static bool s_was_down = false;
static struct timespec s_down_time = {0, 0};
static bool s_long_press_fired = false;
static int s_down_x = 0, s_down_y = 0;
static struct timespec s_last_up_time = {0, 0};
static int s_last_up_x = 0, s_last_up_y = 0;

int touch_init(void) {
    s_num_touch_fds = 0;
    s_was_down = false;
    s_is_down = false;
    s_max_x = 0;
    s_max_y = 0;

    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct input_absinfo abs_x, abs_y;
            bool is_touch = false;

            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0) {
                s_max_x = abs_x.maximum;
                is_touch = true;
            } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > 0) {
                s_max_x = abs_x.maximum;
                is_touch = true;
            }

            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                s_max_y = abs_y.maximum;
                is_touch = true;
            } else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                s_max_y = abs_y.maximum;
                is_touch = true;
            }

            if (is_touch) {
                ioctl(fd, EVIOCGRAB, 1);
                s_touch_fds[s_num_touch_fds++] = fd;
                LOGI("Connected touchscreen device %s (fd %d, max=%dx%d, grabbed)",
                     path, fd, s_max_x, s_max_y);
            } else {
                /* Never grab non-touch devices (e.g. event0 power button!) */
                close(fd);
            }
        }
    }
    if (s_max_x <= 0) s_max_x = DEFAULT_SCREEN_W;
    if (s_max_y <= 0) s_max_y = DEFAULT_SCREEN_H;
    LOGI("Touchscreen initialized: %d device(s), max %dx%d -> screen %dx%d",
         s_num_touch_fds, s_max_x, s_max_y, DEFAULT_SCREEN_W, DEFAULT_SCREEN_H);
    return 0;
}

void touch_cleanup(void) {
    for (int i = 0; i < s_num_touch_fds; i++) {
        if (s_touch_fds[i] >= 0) {
            ioctl(s_touch_fds[i], EVIOCGRAB, 0);
            close(s_touch_fds[i]);
            s_touch_fds[i] = -1;
        }
    }
    s_num_touch_fds = 0;
}

void touch_poll(void) {
    struct input_event ev[64];

    for (int i = 0; i < s_num_touch_fds; i++) {
        int rd = read(s_touch_fds[i], ev, sizeof(ev));
        if (rd <= 0) continue;

        int count = rd / sizeof(struct input_event);
        for (int j = 0; j < count; j++) {
            if (ev[j].type == EV_ABS) {
                if (ev[j].code == ABS_MT_TRACKING_ID) {
                    /* Linux Type B Multitouch: Tracking ID >= 0 is Contact Down, -1 is Lift */
                    s_is_down = (ev[j].value >= 0);
                } else if (ev[j].code == ABS_MT_POSITION_X || ev[j].code == ABS_X) {
                    s_is_down = true;
                    s_raw_x = ev[j].value;
                    if (s_max_x > 0) {
                        s_cur_x = (int)((long long)ev[j].value * DEFAULT_SCREEN_W / s_max_x);
                    } else {
                        s_cur_x = ev[j].value;
                    }
                } else if (ev[j].code == ABS_MT_POSITION_Y || ev[j].code == ABS_Y) {
                    s_is_down = true;
                    s_raw_y = ev[j].value;
                    if (s_max_y > 0) {
                        s_cur_y = (int)((long long)ev[j].value * DEFAULT_SCREEN_H / s_max_y);
                    } else {
                        s_cur_y = ev[j].value;
                    }
                }
            } else if (ev[j].type == EV_KEY) {
                if (ev[j].code == BTN_TOUCH || ev[j].code == BTN_LEFT) {
                    s_is_down = (ev[j].value != 0);
                }
            }
        }
    }

    /* Clamp coordinates safely */
    if (s_cur_x < 0) s_cur_x = 0;
    if (s_cur_x >= DEFAULT_SCREEN_W) s_cur_x = DEFAULT_SCREEN_W - 1;
    if (s_cur_y < 0) s_cur_y = 0;
    if (s_cur_y >= DEFAULT_SCREEN_H) s_cur_y = DEFAULT_SCREEN_H - 1;

    /* Transition: Touch Contact Began */
    if (s_is_down && !s_was_down) {
        s_was_down = true;
        s_touch_dirty = true;
        s_long_press_fired = false;
        s_down_x = s_cur_x;
        s_down_y = s_cur_y;
        clock_gettime(CLOCK_MONOTONIC, &s_down_time);

        LOGI("Touch DOWN at screen (%d, %d) [raw: %d, %d]", s_cur_x, s_cur_y, s_raw_x, s_raw_y);

        /* 1. Emergency Exit Check (Top-Right Corner) */
        if (s_cur_x >= DEFAULT_SCREEN_W - 80 && s_cur_y <= 80) {
            LOGI("Emergency exit triggered at (%d, %d)", s_cur_x, s_cur_y);
            exit(0);
        }

        /* 2. Virtual Keyboard */
        if (vkeyboard_is_visible() && vkeyboard_handle_tap(s_cur_x, s_cur_y)) {
            return;
        }

        /* 3. Double-tap detection or normal mouse down */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_since_up = (now.tv_sec - s_last_up_time.tv_sec) * 1000 + (now.tv_nsec - s_last_up_time.tv_nsec) / 1000000;
        int tap_dist_sq = (s_cur_x - s_last_up_x) * (s_cur_x - s_last_up_x) + (s_cur_y - s_last_up_y) * (s_cur_y - s_last_up_y);

        if (elapsed_since_up <= 450 && tap_dist_sq < 1600) {
            LOGI("Double-tap detected at (%d, %d)", s_cur_x, s_cur_y);
            user_inject_mouse_dblclk(s_cur_x, s_cur_y);
        } else {
            user_inject_mouse_down(s_cur_x, s_cur_y, s_right_click_toggle);
            s_right_click_toggle = false;
        }
    }
    /* Contact Held: Check Long-Press or Move */
    else if (s_is_down && s_was_down) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - s_down_time.tv_sec) * 1000 + (now.tv_nsec - s_down_time.tv_nsec) / 1000000;

        int dist_sq = (s_cur_x - s_down_x) * (s_cur_x - s_down_x) + (s_cur_y - s_down_y) * (s_cur_y - s_down_y);

        if (!s_long_press_fired && elapsed_ms >= 400 && dist_sq < 256) {
            s_long_press_fired = true;
            s_touch_dirty = true;
            LOGI("Long-press detected at (%d, %d) -> Triggering Right Click (Flag)", s_cur_x, s_cur_y);
            /* Release left and send right click */
            user_inject_mouse_up(s_cur_x, s_cur_y, false);
            user_inject_mouse_down(s_cur_x, s_cur_y, true);
            user_inject_mouse_up(s_cur_x, s_cur_y, true);
        } else if (dist_sq >= 64) {
            user_inject_mouse_move(s_cur_x, s_cur_y);
        }
    }
    /* Transition: Touch Contact Released */
    else if (!s_is_down && s_was_down) {
        s_was_down = false;
        s_touch_dirty = true;
        clock_gettime(CLOCK_MONOTONIC, &s_last_up_time);
        s_last_up_x = s_cur_x;
        s_last_up_y = s_cur_y;
        LOGI("Touch UP at screen (%d, %d)", s_cur_x, s_cur_y);
        if (!s_long_press_fired) {
            user_inject_mouse_up(s_cur_x, s_cur_y, false);
        }
    }
}
#endif
