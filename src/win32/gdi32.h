#ifndef GDI32_H
#define GDI32_H

#include "../common.h"
#include "../pe_loader.h"
#include "win32_types.h"

#define GDI_MAX_DCS     32
#define GDI_MAX_OBJS    256

typedef enum {
    GDI_OBJ_NONE = 0,
    GDI_OBJ_PEN,
    GDI_OBJ_BRUSH,
    GDI_OBJ_BITMAP,
    GDI_OBJ_FONT,
    GDI_OBJ_REGION
} gdi_obj_type_t;

typedef struct {
    gdi_obj_type_t type;
    union {
        struct {
            uint32_t color;
            int width;
            int style;
        } pen;
        struct {
            uint32_t color;
            int style;
        } brush;
        struct {
            int width;
            int height;
            int bpp;
            int stride;
            uint8_t *bits;
            bool is_dib;
            WIN_RGBQUAD palette[256];
        } bitmap;
        struct {
            char face_name[32];
            int height;
            int weight;
            bool italic;
        } font;
    };
} gdi_object_t;

typedef struct {
    bool in_use;
    H_HBITMAP selected_bitmap;
    H_HPEN    selected_pen;
    H_HBRUSH  selected_brush;
    H_HFONT   selected_font;
    uint32_t  text_color;
    uint32_t  bk_color;
    int       bk_mode;        /* 1 = TRANSPARENT, 2 = OPAQUE */
    int       rop2;
    int       cur_x;
    int       cur_y;
    H_HWND    hwnd;           /* Linked window if window DC */
} gdi_dc_t;

void gdi32_init(void);

extern const uint8_t GDI_FONT_8X8[95][8];

/* Internal GDI access for Window Compositor */
H_HANDLE gdi_alloc_obj(gdi_obj_type_t type);
gdi_dc_t *gdi_get_dc(H_HDC hdc);
gdi_object_t *gdi_get_obj(H_HANDLE handle);
H_HDC gdi_create_window_dc(H_HWND hwnd);
void gdi_draw_text(H_HDC hdc, int x, int y, const char *text, int len);

#endif /* GDI32_H */
