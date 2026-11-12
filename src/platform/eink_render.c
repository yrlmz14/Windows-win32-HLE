#include "eink_render.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static HWND s_sim_hwnd = NULL;
static HDC  s_sim_hdc = NULL;
static BITMAPINFO s_bmi;
static uint32_t *s_sim_pixels = NULL;

LRESULT CALLBACK SimWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#else
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#endif

static eink_context_t s_eink;

int eink_init(int w, int h) {
    memset(&s_eink, 0, sizeof(s_eink));
    s_eink.width = w > 0 ? w : DEFAULT_SCREEN_W;
    s_eink.height = h > 0 ? h : DEFAULT_SCREEN_H;
    s_eink.stride = s_eink.width;
    s_eink.fbfd = -1;
    s_eink.canvas = (uint8_t *)malloc(s_eink.width * s_eink.height);
    if (!s_eink.canvas) return -1;
    memset(s_eink.canvas, EINK_WHITE, s_eink.width * s_eink.height);

#ifdef _WIN32
    s_eink.is_sim = true;
    s_sim_pixels = (uint32_t *)malloc(s_eink.width * s_eink.height * sizeof(uint32_t));

    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = s_eink.width;
    s_bmi.bmiHeader.biHeight = -s_eink.height; /* Top-down */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;

    if (!getenv("HEADLESS_SIM")) {
        WNDCLASSA wc = {0};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = SimWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "KindleWin32Sim";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);

        int win_w = s_eink.width;
        int win_h = s_eink.height;

        s_sim_hwnd = CreateWindowExA(0, "KindleWin32Sim", "Win32 HLE Runner (MinGW)",
                                     WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                     100, 50, win_w, win_h, NULL, NULL, GetModuleHandle(NULL), NULL);
        s_sim_hdc = GetDC(s_sim_hwnd);
    }
#else
    s_eink.fbfd = open("/dev/fb0", O_RDWR);
    if (s_eink.fbfd >= 0) {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        if (ioctl(s_eink.fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(s_eink.fbfd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            s_eink.fb_bpp = vinfo.bits_per_pixel;
            s_eink.fb_stride = finfo.line_length;
            s_eink.fb_size = finfo.smem_len;
            s_eink.fb_width = vinfo.xres;
            s_eink.fb_height = vinfo.yres;
            s_eink.fb_xoffset = vinfo.xoffset;
            s_eink.fb_yoffset = vinfo.yoffset;

            if (s_eink.fb_width <= 0) s_eink.fb_width = s_eink.width;
            if (s_eink.fb_height <= 0) s_eink.fb_height = s_eink.height;

            s_eink.fb_mmap = (uint8_t *)mmap(0, s_eink.fb_size, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, s_eink.fbfd, 0);
            if (s_eink.fb_mmap != MAP_FAILED) {
                LOGI("Kindle Framebuffer /dev/fb0 mapped (%dx%d, %d bpp, stride %d, offset %dx%d, size %zu)",
                     s_eink.fb_width, s_eink.fb_height, s_eink.fb_bpp, s_eink.fb_stride,
                     s_eink.fb_xoffset, s_eink.fb_yoffset, s_eink.fb_size);
            } else {
                LOGW("Framebuffer mmap failed, running in sim mode");
                s_eink.fb_mmap = NULL;
                s_eink.is_sim = true;
            }
        } else {
            LOGW("Failed to query fb0 screeninfo");
            s_eink.is_sim = true;
        }
    } else {
        LOGW("Could not open /dev/fb0 (headless/sim mode)");
        s_eink.is_sim = true;
    }
#endif
    return 0;
}

void eink_cleanup(void) {
    if (s_eink.canvas) { free(s_eink.canvas); s_eink.canvas = NULL; }
#ifdef _WIN32
    if (s_sim_pixels) { free(s_sim_pixels); s_sim_pixels = NULL; }
    if (s_sim_hwnd) { ReleaseDC(s_sim_hwnd, s_sim_hdc); DestroyWindow(s_sim_hwnd); }
#else
    if (s_eink.fb_mmap && s_eink.fb_size > 0) {
        munmap(s_eink.fb_mmap, s_eink.fb_size);
        s_eink.fb_mmap = NULL;
    }
    if (s_eink.fbfd >= 0) { close(s_eink.fbfd); s_eink.fbfd = -1; }
#endif
}

void eink_clear(uint8_t color) {
    if (s_eink.canvas) memset(s_eink.canvas, color, s_eink.width * s_eink.height);
}

void eink_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (!s_eink.canvas) return;
    for (int cy = y; cy < y + h; cy++) {
        if (cy < 0 || cy >= s_eink.height) continue;
        for (int cx = x; cx < x + w; cx++) {
            if (cx < 0 || cx >= s_eink.width) continue;
            s_eink.canvas[(cy * s_eink.stride) + cx] = color;
        }
    }
}

void eink_draw_rect(int x, int y, int w, int h, uint8_t color, int thickness) {
    eink_fill_rect(x, y, w, thickness, color);
    eink_fill_rect(x, y + h - thickness, w, thickness, color);
    eink_fill_rect(x, y, thickness, h, color);
    eink_fill_rect(x + w - thickness, y, thickness, h, color);
}

static const uint8_t EINK_FONT_8X8[95][8] = {
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
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* '8' */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* '9' */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* ':' */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, /* ';' */
    {0x0E,0x18,0x30,0x60,0x30,0x18,0x0E,0x00}, /* '<' */
    {0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '=' */
    {0x70,0x18,0x0C,0x06,0x0C,0x18,0x70,0x00}, /* '>' */
    {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00}, /* '?' */
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, /* '@' */
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* 'A' */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* 'B' */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* 'C' */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* 'D' */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* 'E' */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* 'F' */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3A,0x00}, /* 'G' */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'I' */
    {0x0E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, /* 'J' */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* 'K' */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* 'L' */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* 'M' */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, /* 'N' */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 'O' */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* 'P' */
    {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x0E,0x00}, /* 'Q' */
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, /* 'R' */
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
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, /* '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, /* 'a' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* 'b' */
    {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00}, /* 'c' */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* 'd' */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* 'e' */
    {0x1C,0x30,0x78,0x30,0x30,0x30,0x30,0x00}, /* 'f' */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, /* 'g' */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00}, /* 'j' */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* 'm' */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* 'o' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* 'p' */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07}, /* 'q' */
    {0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00}, /* 'r' */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* 's' */
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0C,0x00}, /* 't' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* 'u' */
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

void eink_draw_string(int x, int y, const char *str, uint8_t color) {
    if (!s_eink.canvas || !str) return;
    int len = (int)strlen(str);

    for (int i = 0; i < len; i++) {
        char ch = str[i];
        if (ch < 32 || ch > 126) ch = ' ';
        const uint8_t *glyph = EINK_FONT_8X8[ch - 32];

        for (int gy = 0; gy < 8; gy++) {
            int py = y + gy;
            if (py < 0 || py >= s_eink.height) continue;
            uint8_t row = glyph[gy];

            for (int gx = 0; gx < 8; gx++) {
                int px = x + (i * 8) + gx;
                if (px < 0 || px >= s_eink.width) continue;
                if (row & (0x80 >> gx)) {
                    s_eink.canvas[(py * s_eink.stride) + px] = color;
                }
            }
        }
    }
}

void eink_present(int mode) {
    if (!s_eink.canvas) return;

#ifdef _WIN32
    const char *dump_path = getenv("DUMP_BMP");
    if (dump_path && dump_path[0]) {
        bool has_content = false;
        for (int i = 0; i < s_eink.width * s_eink.height; i++) {
            if (s_eink.canvas[i] != EINK_WHITE) { has_content = true; break; }
        }
        if (has_content) {
            char tmp_path[512];
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dump_path);
            FILE *f = fopen(tmp_path, "wb");
            if (f) {
                int w = s_eink.width;
                int h = s_eink.height;
                uint32_t row_bytes = (uint32_t)w * 3;
                uint32_t img_size = row_bytes * (uint32_t)h;
                uint8_t bmp_hdr[54] = {0};
                bmp_hdr[0] = 'B'; bmp_hdr[1] = 'M';
                *(uint32_t *)(bmp_hdr + 2) = 54 + img_size;
                *(uint32_t *)(bmp_hdr + 10) = 54;
                *(uint32_t *)(bmp_hdr + 14) = 40;
                *(int32_t *)(bmp_hdr + 18) = w;
                *(int32_t *)(bmp_hdr + 22) = -h;
                *(uint16_t *)(bmp_hdr + 26) = 1;
                *(uint16_t *)(bmp_hdr + 28) = 24;
                *(uint32_t *)(bmp_hdr + 34) = img_size;
                fwrite(bmp_hdr, 1, 54, f);
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        uint8_t g = s_eink.canvas[y * s_eink.stride + x];
                        uint8_t px[3] = { g, g, g };
                        fwrite(px, 1, 3, f);
                    }
                }
                fclose(f);
                remove(dump_path);
                rename(tmp_path, dump_path);
            }
        }
    }
    if (s_sim_pixels && s_sim_hdc) {
        for (int i = 0; i < s_eink.width * s_eink.height; i++) {
            uint8_t g = s_eink.canvas[i];
            s_sim_pixels[i] = (0xFF << 24) | (g << 16) | (g << 8) | g;
        }
        RECT rc;
        GetClientRect(s_sim_hwnd, &rc);
        StretchDIBits(s_sim_hdc, 0, 0, rc.right, rc.bottom,
                      0, 0, s_eink.width, s_eink.height,
                      s_sim_pixels, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    }
#else
    if (s_eink.fb_mmap && s_eink.fbfd >= 0) {
        int fw = s_eink.fb_width;
        int fh = s_eink.fb_height;
        uint8_t *fb_base = s_eink.fb_mmap + (s_eink.fb_yoffset * s_eink.fb_stride) + (s_eink.fb_xoffset * (s_eink.fb_bpp / 8));

        if (s_eink.fb_bpp == 8) {
            for (int y = 0; y < fh; y++) {
                int src_y = (y * s_eink.height) / fh;
                uint8_t *src = &s_eink.canvas[src_y * s_eink.width];
                uint8_t *dst = &fb_base[y * s_eink.fb_stride];
                for (int x = 0; x < fw; x++) {
                    int src_x = (x * s_eink.width) / fw;
                    dst[x] = src[src_x];
                }
            }
        } else if (s_eink.fb_bpp == 16) {
            for (int y = 0; y < fh; y++) {
                int src_y = (y * s_eink.height) / fh;
                uint8_t *src_row = &s_eink.canvas[src_y * s_eink.width];
                uint16_t *dst = (uint16_t *)&fb_base[y * s_eink.fb_stride];
                for (int x = 0; x < fw; x++) {
                    int src_x = (x * s_eink.width) / fw;
                    uint8_t g = src_row[src_x];
                    uint16_t rgb565 = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
                    dst[x] = rgb565;
                }
            }
        } else if (s_eink.fb_bpp == 32 || s_eink.fb_bpp == 24) {
            for (int y = 0; y < fh; y++) {
                int src_y = (y * s_eink.height) / fh;
                uint8_t *src_row = &s_eink.canvas[src_y * s_eink.width];
                uint32_t *dst = (uint32_t *)&fb_base[y * s_eink.fb_stride];
                for (int x = 0; x < fw; x++) {
                    int src_x = (x * s_eink.width) / fw;
                    uint8_t g = src_row[src_x];
                    dst[x] = (0xFF << 24) | (g << 16) | (g << 8) | g;
                }
            }
        }

        /* Rate limit hardware E-Ink updates to prevent EPDC driver lockups */
        static struct timespec s_last_update = {0, 0};
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - s_last_update.tv_sec) * 1000 + (now.tv_nsec - s_last_update.tv_nsec) / 1000000;

        if (mode == EINK_REFRESH_FULL) {
            s_last_update = now;
            system("./fbink -q -s -f 2>/dev/null || fbink -q -s -f 2>/dev/null || /usr/sbin/eips '' 2>/dev/null");
        } else if (elapsed_ms >= 60) {
            s_last_update = now;
            system("./fbink -q -s 2>/dev/null || fbink -q -s 2>/dev/null");
        }
    }
#endif
}

uint8_t *eink_get_canvas(int *out_w, int *out_h, int *out_stride) {
    if (out_w) *out_w = s_eink.width;
    if (out_h) *out_h = s_eink.height;
    if (out_stride) *out_stride = s_eink.stride;
    return s_eink.canvas;
}
