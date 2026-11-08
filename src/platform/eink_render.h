#ifndef EINK_RENDER_H
#define EINK_RENDER_H

#include "../common.h"

#define EINK_REFRESH_FAST 0  /* Partial fast update (DU / A2 waveform: sub-100ms) */
#define EINK_REFRESH_FULL 1  /* Full grayscale refresh (GC16 / GLR16: clears ghosting) */

typedef struct {
    int fbfd;
    uint8_t *canvas;      /* Main 1236 x 1648 8-bit grayscale canvas */
    uint8_t *fb_mmap;     /* /dev/fb0 memory map */
    size_t fb_size;
    int width;
    int height;
    int stride;
    int fb_bpp;
    int fb_stride;
    int fb_width;
    int fb_height;
    int fb_xoffset;
    int fb_yoffset;
    bool is_sim;
} eink_context_t;

int  eink_init(int w, int h);
void eink_cleanup(void);
void eink_clear(uint8_t color);
void eink_fill_rect(int x, int y, int w, int h, uint8_t color);
void eink_draw_rect(int x, int y, int w, int h, uint8_t color, int thickness);
void eink_draw_string(int x, int y, const char *str, uint8_t color);
void eink_present(int mode);

uint8_t *eink_get_canvas(int *out_w, int *out_h, int *out_stride);

#endif /* EINK_RENDER_H */
