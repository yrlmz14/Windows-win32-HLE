#ifndef _CNFGKINDLEDRIVER_C
#define _CNFGKINDLEDRIVER_C

#include "CNFG.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <time.h>

extern void HandleMotion(int x, int y, int mask);
extern void HandleMotionRel(int x, int y, int mask);
extern void HandleButton(int x, int y, int button, int bDown);
extern void HandleKey(int keycode, int bDown);
extern void KindleSendKey(int scancode);

static int s_fbfd = -1;
static uint8_t *s_fbmmap = NULL;
static size_t s_fbsize = 0;
static int s_fb_width = 1236;
static int s_fb_height = 1648;
static int s_fb_stride = 1236;
static int s_fb_bpp = 8;
static int s_fb_xoffset = 0;
static int s_fb_yoffset = 0;

static short s_vga_w = 640;
static short s_vga_h = 480;

/* Touch input devices */
static int s_touch_fds[8];
static int s_num_touch_fds = 0;
static int s_touch_max_x = 1236;
static int s_touch_max_y = 1648;
static int s_touch_cur_x = 0;
static int s_touch_cur_y = 0;
static bool s_raw_contact = false;
static bool s_touch_down = false;
static struct timespec s_down_time;
static bool s_long_press_fired = false;
static bool s_has_dragged = false;

/* Virtual cursor tracking for direct absolute-to-relative mouse events */
static int s_cursor_x = 320;
static int s_cursor_y = 240;
static int s_acc_dx = 0;
static int s_acc_dy = 0;
static bool s_cursor_inited = false;

static void kindle_send_pointer_delta(int target_x, int target_y, int is_dragging) {
    s_acc_dx += (target_x - s_cursor_x);
    s_acc_dy += (target_y - s_cursor_y);
    s_cursor_x = target_x;
    s_cursor_y = target_y;

    /* Windows 95 standard PS/2 driver has a 2:1 pixel-to-mickey gear ratio */
    int send_dx = s_acc_dx / 2;
    int send_dy = s_acc_dy / 2;
    s_acc_dx %= 2;
    s_acc_dy %= 2;

    if (send_dx != 0 || send_dy != 0) {
        HandleMotionRel(send_dx, send_dy, is_dragging ? 1 : 0);
    }
}

/* Emergency exit gesture: triple tap top-right corner */
static int s_exit_tap_count = 0;
static struct timespec s_last_exit_tap = {0, 0};

/* Rate-limiting E-Ink hardware updates */
static struct timespec s_last_eink_update = {0, 0};

void CNFGChangeWindowTitle(const char *windowtitle) {
    (void)windowtitle;
}

int CNFGSetup(const char *name_of_window, int width, int height) {
    (void)name_of_window;
    s_vga_w = width > 0 ? width : 640;
    s_vga_h = height > 0 ? height : 480;

    /* 1. Open Framebuffer */
    s_fbfd = open("/dev/fb0", O_RDWR);
    if (s_fbfd >= 0) {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;

        if (ioctl(s_fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(s_fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            s_fb_width = vinfo.xres;
            s_fb_height = vinfo.yres;
            s_fb_stride = finfo.line_length;
            s_fb_bpp = vinfo.bits_per_pixel;
            s_fb_xoffset = vinfo.xoffset;
            s_fb_yoffset = vinfo.yoffset;
            s_fbsize = finfo.smem_len;

            s_fbmmap = (uint8_t *)mmap(NULL, s_fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, s_fbfd, 0);
            if (s_fbmmap == MAP_FAILED) {
                s_fbmmap = NULL;
            }
        }
    }

    fprintf(stderr, "[KINDLE-FB] Initialized framebuffer: %dx%d (%d bpp, stride=%d, mmap=%p)\n",
            s_fb_width, s_fb_height, s_fb_bpp, s_fb_stride, s_fbmmap);

    /* 2. Open Touchscreen */
    s_num_touch_fds = 0;
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct input_absinfo abs_x, abs_y;
            bool is_touch = false;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0) {
                s_touch_max_x = abs_x.maximum;
                is_touch = true;
            } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > 0) {
                s_touch_max_x = abs_x.maximum;
                is_touch = true;
            }
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                s_touch_max_y = abs_y.maximum;
                is_touch = true;
            } else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                s_touch_max_y = abs_y.maximum;
                is_touch = true;
            }

            if (is_touch) {
                ioctl(fd, EVIOCGRAB, 1);
                s_touch_fds[s_num_touch_fds++] = fd;
                fprintf(stderr, "[KINDLE-FB] Grabbed touch device %s (max %dx%d)\n",
                        path, s_touch_max_x, s_touch_max_y);
            } else {
                close(fd);
            }
        }
    }

    return 0;
}

void CNFGSetupFullscreen(const char *WindowName, int screen_number) {
    (void)screen_number;
    CNFGSetup(WindowName, s_vga_w, s_vga_h);
}

void CNFGGetDimensions(short *x, short *y) {
    if (x) *x = s_vga_w;
    if (y) *y = s_vga_h;
}

void CNFGUpdateScreenWithBitmap(uint32_t *data, int w, int h) {
    if (!data || !s_fbmmap) return;

    if (w > 0 && h > 0) {
        s_vga_w = (short)w;
        s_vga_h = (short)h;
    }

    /* Center and scale VGA frame (w x h) into Kindle display (s_fb_width x s_fb_height) */
    int dst_w = s_fb_width;
    int dst_h = s_fb_height;
    uint8_t *fb_base = s_fbmmap + (s_fb_yoffset * s_fb_stride) + (s_fb_xoffset * (s_fb_bpp / 8));

    if (s_fb_bpp == 8) {
        /* 8-bit E-Ink grayscale conversion */
        for (int y = 0; y < dst_h; y++) {
            int src_y = (y * h) / dst_h;
            uint32_t *src_row = &data[src_y * w];
            uint8_t *dst_row = &fb_base[y * s_fb_stride];

            for (int x = 0; x < dst_w; x++) {
                int src_x = (x * w) / dst_w;
                uint32_t px = src_row[src_x];
                uint8_t r = (px >> 16) & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = px & 0xFF;
                /* ITU-R BT.601 grayscale */
                uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                dst_row[x] = gray;
            }
        }
    } else if (s_fb_bpp == 16) {
        /* RGB565 */
        for (int y = 0; y < dst_h; y++) {
            int src_y = (y * h) / dst_h;
            uint32_t *src_row = &data[src_y * w];
            uint16_t *dst_row = (uint16_t *)&fb_base[y * s_fb_stride];

            for (int x = 0; x < dst_w; x++) {
                int src_x = (x * w) / dst_w;
                uint32_t px = src_row[src_x];
                uint16_t rgb565 = (((px >> 19) & 0x1F) << 11) |
                                  (((px >> 10) & 0x3F) << 5)  |
                                  ((px >> 3) & 0x1F);
                dst_row[x] = rgb565;
            }
        }
    } else if (s_fb_bpp == 32) {
        /* 32-bit ARGB / XRGB */
        for (int y = 0; y < dst_h; y++) {
            int src_y = (y * h) / dst_h;
            uint32_t *src_row = &data[src_y * w];
            uint32_t *dst_row = (uint32_t *)&fb_base[y * s_fb_stride];

            for (int x = 0; x < dst_w; x++) {
                int src_x = (x * w) / dst_w;
                dst_row[x] = src_row[src_x];
            }
        }
    }

    /* Rate-limit E-Ink screen flash (max 15 FPS) to keep display responsive without locking hardware */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - s_last_eink_update.tv_sec) * 1000 +
                      (now.tv_nsec - s_last_eink_update.tv_nsec) / 1000000;
    if (elapsed_ms >= 65) {
        s_last_eink_update = now;
        system("/var/tmp/win32/fbink -q -s 2>/dev/null || ./fbink -q -s 2>/dev/null || /mnt/us/extensions/win32/bin/fbink -q -s 2>/dev/null || fbink -q -s 2>/dev/null || /usr/sbin/eips '' 2>/dev/null || eips '' 2>/dev/null");
    }
}

void CNFGBlitImage(uint32_t *data, int x, int y, int w, int h) {
    (void)x; (void)y;
    CNFGUpdateScreenWithBitmap(data, w, h);
}

void CNFGClearFrame() {}
void CNFGSwapBuffers() {}

int CNFGHandleInput() {
    struct input_event ev;
    for (int i = 0; i < s_num_touch_fds; i++) {
        while (read(s_touch_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_TRACKING_ID) {
                    /* Linux Multitouch Type B: Tracking ID >= 0 is Contact Down, -1 is Lift */
                    s_raw_contact = (ev.value >= 0);
                } else if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                    s_touch_cur_x = ev.value;
                    s_raw_contact = true;
                } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
                    s_touch_cur_y = ev.value;
                    s_raw_contact = true;
                }
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_TOUCH || ev.code == BTN_LEFT) {
                    s_raw_contact = (ev.value != 0);
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);

                /* Map Kindle touchscreen (s_touch_max_x x s_touch_max_y) directly to VGA display (s_vga_w x s_vga_h) */
                int target_x = (s_touch_max_x > 0) ? (s_touch_cur_x * s_vga_w) / s_touch_max_x : s_touch_cur_x;
                int target_y = (s_touch_max_y > 0) ? (s_touch_cur_y * s_vga_h) / s_touch_max_y : s_touch_cur_y;
                if (target_x < 0) target_x = 0; if (target_x >= s_vga_w) target_x = s_vga_w - 1;
                if (target_y < 0) target_y = 0; if (target_y >= s_vga_h) target_y = s_vga_h - 1;

                if (!s_cursor_inited) {
                    s_cursor_x = s_vga_w / 2;
                    s_cursor_y = s_vga_h / 2;
                    s_cursor_inited = true;
                }

                if (s_raw_contact && !s_touch_down) {
                    /* --- Contact Began (Finger Touched Screen) --- */
                    s_touch_down = true;
                    s_down_time = now;
                    s_long_press_fired = false;
                    s_has_dragged = false;

                    /* 1. Emergency Exit Check: Triple-tap top-right corner */
                    if (s_touch_max_x > 0 && s_touch_cur_x > (s_touch_max_x - 180) && s_touch_cur_y < 180) {
                        long tap_elapsed_ms = (now.tv_sec - s_last_exit_tap.tv_sec) * 1000 +
                                              (now.tv_nsec - s_last_exit_tap.tv_nsec) / 1000000;
                        if (tap_elapsed_ms < 1500) {
                            s_exit_tap_count++;
                            if (s_exit_tap_count >= 3) {
                                fprintf(stderr, "[KINDLE-FB] Exit gesture triggered (triple tap top-right). Exiting to Kindle OS...\n");
                                return 0;
                            }
                        } else {
                            s_exit_tap_count = 1;
                        }
                        s_last_exit_tap = now;
                        continue;
                    }

                    /* 2. Instant ENTER Key Check: Top-left corner */
                    if (s_touch_cur_x < 180 && s_touch_cur_y < 180) {
                        fprintf(stderr, "[KINDLE-FB] Hot Corner: Top-Left tapped -> Injecting ENTER key (scancode 0x1C)\n");
                        KindleSendKey(0x1C);
                        continue;
                    }

                    /* 3. Direct Pointer Teleportation (Exact Target Under Finger) */
                    kindle_send_pointer_delta(target_x, target_y, 0);
                    HandleButtonRel(0, 0, 1, 1); /* Left click down */

                } else if (s_raw_contact && s_touch_down) {
                    /* --- Contact Held (Dragging Pointer) --- */
                    int dist_x = abs(target_x - s_cursor_x);
                    int dist_y = abs(target_y - s_cursor_y);

                    if (dist_x > 2 || dist_y > 2) {
                        s_has_dragged = true;
                    }

                    kindle_send_pointer_delta(target_x, target_y, 1);

                    /* Long-press check -> right click ONLY if stationary */
                    long down_ms = (now.tv_sec - s_down_time.tv_sec) * 1000 +
                                   (now.tv_nsec - s_down_time.tv_nsec) / 1000000;
                    if (!s_has_dragged && !s_long_press_fired && down_ms >= 600) {
                        s_long_press_fired = true;
                        HandleButtonRel(0, 0, 1, 0); /* Release left */
                        HandleButtonRel(0, 0, 3, 1); /* Right click down */
                        HandleButtonRel(0, 0, 3, 0); /* Right click up */
                        fprintf(stderr, "[KINDLE-FB] Stationary long-press right click at (%d, %d)\n", target_x, target_y);
                    }

                } else if (!s_raw_contact && s_touch_down) {
                    /* --- Contact Released (Finger Lifted) --- */
                    s_touch_down = false;
                    HandleButtonRel(0, 0, 1, 0); /* Left click up */
                }
            }
        }
    }
    return 1;
}

void CNFGSetMousePosition(int x, int y) { (void)x; (void)y; }
void CNFGConfineMouse(int confined) { (void)confined; }
void CNFGSetCursor(CNFGCursorShape shape) { (void)shape; }
void CNFGTackSegment(short x1, short y1, short x2, short y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void CNFGTackRectangle(short x1, short y1, short x2, short y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void CNFGTackPoly(RDPoint *points, int verts) { (void)points; (void)verts; }
void CNFGTackPixel(short x1, short y1) { (void)x1; (void)y1; }

void CNFGTearDown() {
    if (s_fbmmap && s_fbsize > 0) {
        munmap(s_fbmmap, s_fbsize);
        s_fbmmap = NULL;
    }
    if (s_fbfd >= 0) {
        close(s_fbfd);
        s_fbfd = -1;
    }
    for (int i = 0; i < s_num_touch_fds; i++) {
        if (s_touch_fds[i] >= 0) close(s_touch_fds[i]);
    }
    s_num_touch_fds = 0;
}

#endif /* _CNFGKINDLEDRIVER_C */
