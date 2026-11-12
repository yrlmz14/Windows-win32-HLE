#include "gdi32.h"
#include "win32_dispatch.h"
#include <math.h>

void win32_register_api(const char *dll, const char *name, uint16_t ordinal, hle_api_fn handler, uint8_t arg_count);

static gdi_dc_t s_dcs[GDI_MAX_DCS];
static gdi_object_t s_objs[GDI_MAX_OBJS];

/* Standard 8x8 font bitmap for GDI TextOut */
const uint8_t GDI_FONT_8X8[95][8] = {
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

H_HANDLE gdi_alloc_obj(gdi_obj_type_t type) {
    for (int i = 1; i < GDI_MAX_OBJS; i++) {
        if (s_objs[i].type == GDI_OBJ_NONE) {
            memset(&s_objs[i], 0, sizeof(gdi_object_t));
            s_objs[i].type = type;
            return (H_HANDLE)(0x2000 + i);
        }
    }
    return 0;
}

#define alloc_gdi_obj gdi_alloc_obj

gdi_object_t *gdi_get_obj(H_HANDLE handle) {
    uint32_t idx = (uint32_t)handle - 0x2000;
    if (idx > 0 && idx < GDI_MAX_OBJS && s_objs[idx].type != GDI_OBJ_NONE) {
        return &s_objs[idx];
    }
    return NULL;
}

static H_HDC alloc_dc(void) {
    for (int i = 1; i < GDI_MAX_DCS; i++) {
        if (!s_dcs[i].in_use) {
            memset(&s_dcs[i], 0, sizeof(gdi_dc_t));
            s_dcs[i].in_use = true;
            s_dcs[i].text_color = 0x000000; /* Black */
            s_dcs[i].bk_color   = 0xFFFFFF; /* White */
            s_dcs[i].bk_mode    = 2;        /* OPAQUE */
            s_dcs[i].rop2       = 13;       /* R2_COPYPEN */
            return (H_HDC)(0x3000 + i);
        }
    }
    return 0;
}

gdi_dc_t *gdi_get_dc(H_HDC hdc) {
    uint32_t idx = (uint32_t)hdc - 0x3000;
    if (idx > 0 && idx < GDI_MAX_DCS && s_dcs[idx].in_use) {
        return &s_dcs[idx];
    }
    return NULL;
}

H_HDC gdi_create_window_dc(H_HWND hwnd) {
    H_HDC hdc = alloc_dc();
    if (hdc) {
        gdi_dc_t *dc = gdi_get_dc(hdc);
        dc->hwnd = hwnd;
    }
    return hdc;
}

/* API Handlers */
static uint32_t gdi_CreateCompatibleDC(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    H_HDC hdc = alloc_dc();
    LOGD("CreateCompatibleDC -> 0x%08X", hdc);
    return (uint32_t)hdc;
}

static uint32_t gdi_DeleteDC(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    uint32_t idx = (uint32_t)hdc - 0x3000;
    if (idx > 0 && idx < GDI_MAX_DCS) {
        s_dcs[idx].in_use = false;
        return 1;
    }
    return 0;
}

static uint32_t gdi_CreateCompatibleBitmap(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int w = (int)args[1];
    int h = (int)args[2];
    if (w <= 0 || h <= 0) return 0;

    H_HANDLE hbm = alloc_gdi_obj(GDI_OBJ_BITMAP);
    if (!hbm) return 0;

    gdi_object_t *obj = gdi_get_obj(hbm);
    obj->bitmap.width = w;
    obj->bitmap.height = h;
    obj->bitmap.bpp = 32;
    obj->bitmap.stride = w * 4;
    obj->bitmap.bits = (uint8_t *)calloc(w * h, 4);

    LOGD("CreateCompatibleBitmap (%dx%d) -> 0x%08X", w, h, hbm);
    return (uint32_t)hbm;
}

static uint32_t gdi_CreateDIBSection(win32_process_t *proc, uint32_t *args) {
    /* H_HDC hdc = (H_HDC)args[0]; */
    uint32_t pbmi = args[1];
    /* uint32_t usage = args[2]; */
    uint32_t ppvBits = args[3];

    WIN_BITMAPINFO *bmi = (WIN_BITMAPINFO *)guest_va_to_host(&proc->mem, pbmi);
    if (!bmi) return 0;

    int w = bmi->bmiHeader.biWidth;
    int h = bmi->bmiHeader.biHeight;
    int bpp = bmi->bmiHeader.biBitCount;
    if (h < 0) h = -h; /* Top-down */

    H_HANDLE hbm = alloc_gdi_obj(GDI_OBJ_BITMAP);
    if (!hbm) return 0;

    gdi_object_t *obj = gdi_get_obj(hbm);
    obj->bitmap.width = w;
    obj->bitmap.height = h;
    obj->bitmap.bpp = bpp;
    obj->bitmap.is_dib = true;
    obj->bitmap.stride = ((w * bpp + 31) / 32) * 4;
    obj->bitmap.bits = (uint8_t *)calloc(obj->bitmap.stride * h, 1);

    if (bpp <= 8 && bmi->bmiHeader.biClrUsed > 0) {
        memcpy(obj->bitmap.palette, bmi->bmiColors, bmi->bmiHeader.biClrUsed * sizeof(WIN_RGBQUAD));
    }

    /* Allocate guest memory buffer for ppvBits */
    uint32_t guest_bits = 0x03000000 + ((uint32_t)hbm * 0x10000);
    guest_mem_commit(&proc->mem, guest_bits, obj->bitmap.stride * h);
    if (ppvBits) {
        x86_mem_write32(&proc->cpu, ppvBits, guest_bits);
    }

    LOGD("CreateDIBSection (%dx%d, %d bpp) -> 0x%08X", w, h, bpp, hbm);
    return (uint32_t)hbm;
}

static uint32_t gdi_CreateSolidBrush(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = alloc_gdi_obj(GDI_OBJ_BRUSH);
    if (h) {
        gdi_object_t *obj = gdi_get_obj(h);
        obj->brush.color = args[0];
        obj->brush.style = 0; /* BS_SOLID */
    }
    return (uint32_t)h;
}

static uint32_t gdi_CreatePen(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = alloc_gdi_obj(GDI_OBJ_PEN);
    if (h) {
        gdi_object_t *obj = gdi_get_obj(h);
        obj->pen.style = (int)args[0];
        obj->pen.width = (int)args[1];
        obj->pen.color = args[2];
    }
    return (uint32_t)h;
}

static uint32_t gdi_SelectObject(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    H_HANDLE hobj = (H_HANDLE)args[1];

    gdi_dc_t *dc = gdi_get_dc(hdc);
    gdi_object_t *obj = gdi_get_obj(hobj);
    if (!dc || !obj) return 0;

    H_HANDLE prev = 0;
    switch (obj->type) {
        case GDI_OBJ_BITMAP:
            prev = (H_HANDLE)dc->selected_bitmap;
            dc->selected_bitmap = (H_HBITMAP)hobj;
            break;
        case GDI_OBJ_PEN:
            prev = (H_HANDLE)dc->selected_pen;
            dc->selected_pen = (H_HPEN)hobj;
            break;
        case GDI_OBJ_BRUSH:
            prev = (H_HANDLE)dc->selected_brush;
            dc->selected_brush = (H_HBRUSH)hobj;
            break;
        case GDI_OBJ_FONT:
            prev = (H_HANDLE)dc->selected_font;
            dc->selected_font = (H_HFONT)hobj;
            break;
        default:
            break;
    }
    return prev ? (uint32_t)prev : (uint32_t)hobj;
}

static uint32_t gdi_DeleteObject(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HANDLE h = (H_HANDLE)args[0];
    gdi_object_t *obj = gdi_get_obj(h);
    if (obj) {
        if (obj->type == GDI_OBJ_BITMAP && obj->bitmap.bits && !obj->bitmap.is_window_fb) {
            free(obj->bitmap.bits);
            obj->bitmap.bits = NULL;
        }
        obj->type = GDI_OBJ_NONE;
        return 1;
    }
    return 0;
}

static H_HANDLE s_stock_objs[32] = {0};

static uint32_t gdi_GetStockObject(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    uint32_t obj = args[0];
    if (obj < 32 && s_stock_objs[obj]) {
        return (uint32_t)s_stock_objs[obj];
    }
    H_HANDLE h = 0;
    switch (obj) {
        case 0: /* WHITE_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00FFFFFF; o->brush.style = 0; }
            break;
        case 1: /* LTGRAY_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00C0C0C0; o->brush.style = 0; }
            break;
        case 2: /* GRAY_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00808080; o->brush.style = 0; }
            break;
        case 3: /* DKGRAY_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00404040; o->brush.style = 0; }
            break;
        case 4: /* BLACK_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00000000; o->brush.style = 0; }
            break;
        case 5: /* NULL_BRUSH / HOLLOW_BRUSH */
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00FFFFFF; o->brush.style = 1; }
            break;
        case 6: /* WHITE_PEN */
            h = alloc_gdi_obj(GDI_OBJ_PEN);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->pen.color = 0x00FFFFFF; o->pen.width = 1; }
            break;
        case 7: /* BLACK_PEN */
            h = alloc_gdi_obj(GDI_OBJ_PEN);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->pen.color = 0x00000000; o->pen.width = 1; }
            break;
        case 8: /* NULL_PEN */
            h = alloc_gdi_obj(GDI_OBJ_PEN);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->pen.color = 0x00000000; o->pen.width = 0; }
            break;
        case 13: /* SYSTEM_FONT */
        case 17: /* DEFAULT_GUI_FONT */
            h = alloc_gdi_obj(GDI_OBJ_FONT);
            break;
        default:
            h = alloc_gdi_obj(GDI_OBJ_BRUSH);
            if (h) { gdi_object_t *o = gdi_get_obj(h); o->brush.color = 0x00C0C0C0; o->brush.style = 0; }
            break;
    }
    if (obj < 32) s_stock_objs[obj] = h;
    return (uint32_t)h;
}

/* Drawing Blits & Primitives */
static uint32_t gdi_BitBlt(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdcDest = (H_HDC)args[0];
    int nXDest = (int)args[1];
    int nYDest = (int)args[2];
    int nW = (int)args[3];
    int nH = (int)args[4];
    H_HDC hdcSrc = (H_HDC)args[5];
    int nXSrc = (int)args[6];
    int nYSrc = (int)args[7];
    /* uint32_t rop = args[8]; */

    gdi_dc_t *dc_dst = gdi_get_dc(hdcDest);
    gdi_dc_t *dc_src = gdi_get_dc(hdcSrc);
    if (!dc_dst || !dc_src) return 0;

    gdi_object_t *bm_dst = gdi_get_obj((H_HANDLE)dc_dst->selected_bitmap);
    gdi_object_t *bm_src = gdi_get_obj((H_HANDLE)dc_src->selected_bitmap);
    if (!bm_dst || !bm_src || !bm_dst->bitmap.bits || !bm_src->bitmap.bits) return 0;

    /* Copy rectangle between bitmap surfaces */
    for (int y = 0; y < nH; y++) {
        int sy = nYSrc + y;
        int dy = nYDest + y;
        if (sy < 0 || sy >= bm_src->bitmap.height) continue;
        if (dy < 0 || dy >= bm_dst->bitmap.height) continue;

        for (int x = 0; x < nW; x++) {
            int sx = nXSrc + x;
            int dx = nXDest + x;
            if (sx < 0 || sx >= bm_src->bitmap.width) continue;
            if (dx < 0 || dx >= bm_dst->bitmap.width) continue;

            uint32_t pixel = *(uint32_t *)(bm_src->bitmap.bits + (sy * bm_src->bitmap.stride) + (sx * 4));
            *(uint32_t *)(bm_dst->bitmap.bits + (dy * bm_dst->bitmap.stride) + (dx * 4)) = pixel | 0xFF000000;
        }
    }
    return 1;
}

static uint32_t gdi_PatBlt(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    int w = (int)args[3];
    int h = (int)args[4];
    uint32_t rop = args[5];

    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w <= 0 || h <= 0) return 1;

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

    uint32_t fill_color = 0xFFFFFFFF; /* Default white */
    if (rop == 0x00000042) fill_color = 0xFF000000; /* BLACKNESS */
    else if (rop == 0x00FF0062) fill_color = 0xFFFFFFFF; /* WHITENESS */
    else {
        gdi_object_t *br = gdi_get_obj((H_HANDLE)dc->selected_brush);
        if (br) fill_color = br->brush.color | 0xFF000000;
    }

    for (int cy = y; cy < y + h; cy++) {
        if (cy < 0 || cy >= bm->bitmap.height) continue;
        for (int cx = x; cx < x + w; cx++) {
            if (cx < 0 || cx >= bm->bitmap.width) continue;
            *(uint32_t *)(bm->bitmap.bits + (cy * bm->bitmap.stride) + (cx * 4)) = fill_color;
        }
    }
    return 1;
}

static uint32_t gdi_SetPixel(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    uint32_t color = args[3];

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

    if (x >= 0 && x < bm->bitmap.width && y >= 0 && y < bm->bitmap.height) {
        *(uint32_t *)(bm->bitmap.bits + (y * bm->bitmap.stride) + (x * 4)) = color | 0xFF000000;
    }
    return color;
}

static uint32_t gdi_TextOutA(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    const char *str = (const char *)guest_va_to_host(&proc->mem, args[3]);
    int len = (int)args[4];
    if (!str || len <= 0) return 0;

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

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
                } else if (dc->bk_mode == 2) { /* OPAQUE */
                    *(uint32_t *)(bm->bitmap.bits + (py * bm->bitmap.stride) + (px * 4)) = dc->bk_color | 0xFF000000;
                }
            }
        }
    }
    return 1;
}

static uint32_t gdi_SetTextColor(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    uint32_t col = args[1];
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (dc) {
        uint32_t prev = dc->text_color;
        dc->text_color = col;
        return prev;
    }
    return 0;
}

static uint32_t gdi_SetBkColor(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    uint32_t col = args[1];
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (dc) {
        uint32_t prev = dc->bk_color;
        dc->bk_color = col;
        return prev;
    }
    return 0;
}

static uint32_t gdi_SetBkMode(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int mode = (int)args[1];
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (dc) {
        int prev = dc->bk_mode;
        dc->bk_mode = mode;
        return (uint32_t)prev;
    }
    return 0;
}

static uint32_t gdi_SetDIBitsToDevice(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc           = (H_HDC)args[0];
    int xDest           = (int)args[1];
    int yDest           = (int)args[2];
    uint32_t dwWidth    = args[3];
    uint32_t dwHeight   = args[4];
    int xSrc            = (int)args[5];
    int ySrc            = (int)args[6];
    uint32_t uStartScan = args[7];
    uint32_t cScanLines = args[8];
    uint32_t lpvBits    = args[9];
    uint32_t lpbmi      = args[10];
    /* uint32_t fuColorUse = args[11]; */

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;

    gdi_object_t *bm_dst = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm_dst || !bm_dst->bitmap.bits) return 0;

    WIN_BITMAPINFOHEADER bih;
    if (!guest_mem_read(&proc->mem, lpbmi, &bih, sizeof(bih))) return 0;

    int bpp = bih.biBitCount;
    int bi_w = (int)bih.biWidth;
    int bi_h = (int)bih.biHeight;
    bool bottom_up = (bi_h > 0);
    if (bi_h < 0) bi_h = -bi_h;

    /* Palette for indexed modes */
    WIN_RGBQUAD palette[256];
    memset(palette, 0, sizeof(palette));
    if (bpp <= 8) {
        int num_colors = bih.biClrUsed ? (int)bih.biClrUsed : (1 << bpp);
        if (num_colors > 256) num_colors = 256;
        guest_mem_read(&proc->mem, lpbmi + sizeof(WIN_BITMAPINFOHEADER), palette, num_colors * sizeof(WIN_RGBQUAD));
    }

    int src_stride = ((bi_w * bpp + 31) / 32) * 4;
    uint8_t line_buf[2048];

    for (uint32_t scan = 0; scan < cScanLines; scan++) {
        uint32_t src_line_idx = uStartScan + scan;
        if (src_line_idx >= (uint32_t)bi_h) break;

        uint32_t line_addr = lpvBits + scan * src_stride;
        if (!guest_mem_read(&proc->mem, line_addr, line_buf, src_stride < (int)sizeof(line_buf) ? src_stride : (int)sizeof(line_buf))) {
            break;
        }

        int dest_y;
        if (bottom_up) {
            dest_y = yDest + (int)dwHeight - 1 - (int)scan;
        } else {
            dest_y = yDest + (int)scan;
        }

        if (dest_y < 0 || dest_y >= bm_dst->bitmap.height) continue;

        for (uint32_t col = 0; col < dwWidth; col++) {
            int src_x = xSrc + (int)col;
            int dest_x = xDest + (int)col;
            if (src_x < 0 || src_x >= bi_w) continue;
            if (dest_x < 0 || dest_x >= bm_dst->bitmap.width) continue;

            uint32_t color = 0;
            if (bpp == 4) {
                uint8_t byte_val = line_buf[src_x / 2];
                uint8_t c_idx = (src_x % 2 == 0) ? ((byte_val >> 4) & 0x0F) : (byte_val & 0x0F);
                color = (palette[c_idx].rgbRed << 16) | (palette[c_idx].rgbGreen << 8) | palette[c_idx].rgbBlue;
            } else if (bpp == 1) {
                uint8_t byte_val = line_buf[src_x / 8];
                uint8_t c_idx = (byte_val & (0x80 >> (src_x % 8))) ? 1 : 0;
                color = (palette[c_idx].rgbRed << 16) | (palette[c_idx].rgbGreen << 8) | palette[c_idx].rgbBlue;
            } else if (bpp == 8) {
                uint8_t c_idx = line_buf[src_x];
                color = (palette[c_idx].rgbRed << 16) | (palette[c_idx].rgbGreen << 8) | palette[c_idx].rgbBlue;
            } else if (bpp == 24) {
                uint8_t b = line_buf[src_x * 3 + 0];
                uint8_t g = line_buf[src_x * 3 + 1];
                uint8_t r = line_buf[src_x * 3 + 2];
                color = (r << 16) | (g << 8) | b;
            } else if (bpp == 32) {
                color = *(uint32_t *)(&line_buf[src_x * 4]);
            }

            *(uint32_t *)(bm_dst->bitmap.bits + dest_y * bm_dst->bitmap.stride + dest_x * 4) = color | 0xFF000000;
        }
    }

    LOGD("SetDIBitsToDevice: dst=(%d,%d) size=(%ux%u) -> copied %u lines", xDest, yDest, dwWidth, dwHeight, cScanLines);
    return cScanLines;
}

static uint32_t gdi_SetROP2(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int rop = (int)args[1];
    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (dc) {
        int prev = dc->rop2;
        dc->rop2 = rop;
        return (uint32_t)prev;
    }
    return 13; /* R2_COPYPEN */
}

static uint32_t gdi_GetLayout(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0; /* LAYOUT_LTR */
}

static uint32_t gdi_SetLayout(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0;
}

static uint32_t gdi_MoveToEx(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    uint32_t pt_va = args[3];

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;

    if (pt_va) {
        WIN_POINT prev_pt = { .x = dc->cur_x, .y = dc->cur_y };
        guest_mem_write(&proc->mem, pt_va, &prev_pt, sizeof(prev_pt));
    }

    dc->cur_x = x;
    dc->cur_y = y;
    return 1;
}

static uint32_t gdi_LineTo(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x1 = (int)args[1];
    int y1 = (int)args[2];

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;

    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

    uint32_t pen_color = 0xFF000000;
    gdi_object_t *pen = gdi_get_obj((H_HANDLE)dc->selected_pen);
    if (pen) pen_color = pen->pen.color | 0xFF000000;

    int x0 = dc->cur_x;
    int y0 = dc->cur_y;

    /* Bresenham line algorithm */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        if (x0 >= 0 && x0 < bm->bitmap.width && y0 >= 0 && y0 < bm->bitmap.height) {
            *(uint32_t *)(bm->bitmap.bits + y0 * bm->bitmap.stride + x0 * 4) = pen_color;
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }

    dc->cur_x = x1;
    dc->cur_y = y1;
    return 1;
}

static uint32_t gdi_GetDeviceCaps(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    int index = (int)args[1];
    switch (index) {
        case 8:  return DEFAULT_SCREEN_W; /* HORZRES */
        case 10: return DEFAULT_SCREEN_H; /* VERTRES */
        case 12: return 32;               /* BITSPIXEL */
        case 14: return 1;                /* PLANES */
        case 24: return -1;               /* NUMCOLORS */
        case 36: return 72;               /* ASPECTX */
        case 38: return 72;               /* ASPECTY */
        case 40: return 102;              /* ASPECTXY */
        case 88: return 96;               /* LOGPIXELSX */
        case 90: return 96;               /* LOGPIXELSY */
        default: return 1;
    }
}

static uint32_t gdi_GetObjectW(win32_process_t *proc, uint32_t *args) {
    H_HANDLE handle = args[0];
    uint32_t buf_size = args[1];
    uint32_t out_va = args[2];

    gdi_object_t *obj = gdi_get_obj(handle);
    if (!obj) return 0;

    if (obj->type == GDI_OBJ_BITMAP) {
        struct {
            int32_t bmType;
            int32_t bmWidth;
            int32_t bmHeight;
            int32_t bmWidthBytes;
            uint16_t bmPlanes;
            uint16_t bmBitsPixel;
            uint32_t bmBits;
        } bm;
        memset(&bm, 0, sizeof(bm));
        bm.bmWidth = obj->bitmap.width;
        bm.bmHeight = obj->bitmap.height;
        bm.bmWidthBytes = obj->bitmap.stride;
        bm.bmPlanes = 1;
        bm.bmBitsPixel = 32;

        if (!out_va) return sizeof(bm);
        uint32_t copy_sz = buf_size < sizeof(bm) ? buf_size : sizeof(bm);
        guest_mem_write(&proc->mem, out_va, &bm, copy_sz);
        return copy_sz;
    }
    return 0;
}

static uint32_t gdi_PtVisible(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t gdi_SetBrushOrgEx(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t gdi_GetTextMetricsA(win32_process_t *proc, uint32_t *args) {
    uint32_t tm_va = args[1];
    if (tm_va) {
        uint8_t tm_buf[56] = {0};
        int32_t h = 16, asc = 13, desc = 3, avg_w = 8, max_w = 8;
        memcpy(tm_buf + 0, &h, 4);
        memcpy(tm_buf + 4, &asc, 4);
        memcpy(tm_buf + 8, &desc, 4);
        memcpy(tm_buf + 20, &avg_w, 4);
        memcpy(tm_buf + 24, &max_w, 4);
        guest_mem_write(&proc->mem, tm_va, tm_buf, sizeof(tm_buf));
    }
    return 1;
}

static uint32_t gdi_GetTextMetricsW(win32_process_t *proc, uint32_t *args) {
    uint32_t tm_va = args[1];
    if (tm_va) {
        uint8_t tm_buf[60] = {0};
        int32_t h = 16, asc = 13, desc = 3, avg_w = 8, max_w = 8;
        memcpy(tm_buf + 0, &h, 4);
        memcpy(tm_buf + 4, &asc, 4);
        memcpy(tm_buf + 8, &desc, 4);
        memcpy(tm_buf + 20, &avg_w, 4);
        memcpy(tm_buf + 24, &max_w, 4);
        guest_mem_write(&proc->mem, tm_va, tm_buf, sizeof(tm_buf));
    }
    return 1;
}

static uint32_t gdi_GetTextExtentPoint32W(win32_process_t *proc, uint32_t *args) {
    int c = (int)args[2];
    uint32_t size_va = args[3];
    if (size_va && c >= 0) {
        int32_t sz[2] = { c * 8, 16 };
        guest_mem_write(&proc->mem, size_va, sz, sizeof(sz));
    }
    return 1;
}

static uint32_t gdi_CreateFontW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return (uint32_t)gdi_alloc_obj(GDI_OBJ_FONT);
}

static uint32_t gdi_TextOutW(win32_process_t *proc, uint32_t *args) {
    H_HDC hdc = (H_HDC)args[0];
    int x = (int)args[1];
    int y = (int)args[2];
    uint32_t str_va = args[3];
    int len = (int)args[4];
    if (!str_va || len <= 0) return 0;
    uint16_t wbuf[256];
    if (len > 255) len = 255;
    guest_mem_read(&proc->mem, str_va, wbuf, len * 2);

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm || !bm->bitmap.bits) return 0;

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
    return 1;
}

static uint32_t gdi_LineDDA(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

static uint32_t gdi_CreateFontIndirectW(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 0x00002001;
}

static uint32_t gdi_GetTextFaceW(win32_process_t *proc, uint32_t *args) {
    int nCount = (int)args[1];
    uint32_t lpFaceName = args[2];
    static const uint16_t s_face[] = { 'L','u','c','i','d','a',' ','C','o','n','s','o','l','e',0 };
    int len = 14;
    if (lpFaceName && nCount > 0) {
        int to_copy = (nCount <= len) ? (nCount - 1) : len;
        guest_mem_write(&proc->mem, lpFaceName, s_face, (to_copy + 1) * 2);
        return to_copy;
    }
    return len;
}

static uint32_t gdi_dummy_success(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    return 1;
}

void gdi32_init(void) {
    memset(s_dcs, 0, sizeof(s_dcs));
    for (int i = 1; i < GDI_MAX_OBJS; i++) {
        if (s_objs[i].type == GDI_OBJ_BITMAP && s_objs[i].bitmap.bits && !s_objs[i].bitmap.is_window_fb) {
            free(s_objs[i].bitmap.bits);
            s_objs[i].bitmap.bits = NULL;
        }
    }
    memset(s_objs, 0, sizeof(s_objs));
    memset(s_stock_objs, 0, sizeof(s_stock_objs));

    win32_register_api("GDI32.DLL", "GetObjectW", 0, gdi_GetObjectW, 3);
    win32_register_api("GDI32.DLL", "GetObjectA", 0, gdi_GetObjectW, 3);
    win32_register_api("GDI32.DLL", "CreateCompatibleDC", 0, gdi_CreateCompatibleDC, 1);
    win32_register_api("GDI32.DLL", "DeleteDC", 0, gdi_DeleteDC, 1);
    win32_register_api("GDI32.DLL", "CreateCompatibleBitmap", 0, gdi_CreateCompatibleBitmap, 3);
    win32_register_api("GDI32.DLL", "CreateDIBSection", 0, gdi_CreateDIBSection, 6);
    win32_register_api("GDI32.DLL", "CreateSolidBrush", 0, gdi_CreateSolidBrush, 1);
    win32_register_api("GDI32.DLL", "CreatePen", 0, gdi_CreatePen, 3);
    win32_register_api("GDI32.DLL", "SelectObject", 0, gdi_SelectObject, 2);
    win32_register_api("GDI32.DLL", "DeleteObject", 0, gdi_DeleteObject, 1);
    win32_register_api("GDI32.DLL", "GetStockObject", 0, gdi_GetStockObject, 1);

    win32_register_api("GDI32.DLL", "BitBlt", 0, gdi_BitBlt, 9);
    win32_register_api("GDI32.DLL", "PatBlt", 0, gdi_PatBlt, 6);
    win32_register_api("GDI32.DLL", "SetPixel", 0, gdi_SetPixel, 4);
    win32_register_api("GDI32.DLL", "TextOutA", 0, gdi_TextOutA, 5);
    win32_register_api("GDI32.DLL", "TextOutW", 0, gdi_TextOutW, 5);
    win32_register_api("GDI32.DLL", "CreateFontW", 0, gdi_CreateFontW, 14);
    win32_register_api("GDI32.DLL", "CreateFontA", 0, gdi_CreateFontW, 14);
    win32_register_api("GDI32.DLL", "CreateFontIndirectW", 0, gdi_CreateFontIndirectW, 1);
    win32_register_api("GDI32.DLL", "CreateFontIndirectA", 0, gdi_CreateFontIndirectW, 1);
    win32_register_api("GDI32.DLL", "GetTextFaceW", 0, gdi_GetTextFaceW, 3);
    win32_register_api("GDI32.DLL", "GetTextFaceA", 0, gdi_GetTextFaceW, 3);
    win32_register_api("GDI32.DLL", "GetTextCharset", 0, gdi_dummy_success, 1);
    win32_register_api("GDI32.DLL", "StartDocA", 0, gdi_dummy_success, 2);
    win32_register_api("GDI32.DLL", "CreateDCA", 0, gdi_CreateCompatibleDC, 4);
    win32_register_api("GDI32.DLL", "GetTextExtentPoint32W", 0, gdi_GetTextExtentPoint32W, 4);
    win32_register_api("GDI32.DLL", "GetTextExtentPoint32A", 0, gdi_GetTextExtentPoint32W, 4);
    win32_register_api("GDI32.DLL", "GetTextExtentPointW", 0, gdi_GetTextExtentPoint32W, 4);
    win32_register_api("GDI32.DLL", "GetTextExtentPointA", 0, gdi_GetTextExtentPoint32W, 4);
    win32_register_api("GDI32.DLL", "GetTextMetricsW", 0, gdi_GetTextMetricsW, 2);
    win32_register_api("GDI32.DLL", "GetTextMetricsA", 0, gdi_GetTextMetricsA, 2);
    win32_register_api("GDI32.DLL", "PtVisible", 0, gdi_PtVisible, 3);
    win32_register_api("GDI32.DLL", "SetBrushOrgEx", 0, gdi_SetBrushOrgEx, 4);
    win32_register_api("GDI32.DLL", "LineDDA", 0, gdi_LineDDA, 6);
    win32_register_api("GDI32.DLL", "SetTextColor", 0, gdi_SetTextColor, 2);
    win32_register_api("GDI32.DLL", "SetBkColor", 0, gdi_SetBkColor, 2);
    win32_register_api("GDI32.DLL", "SetBkMode", 0, gdi_SetBkMode, 2);
    win32_register_api("GDI32.DLL", "GetDeviceCaps", 0, gdi_GetDeviceCaps, 2);

    win32_register_api("GDI32.DLL", "SetROP2", 0, gdi_SetROP2, 2);
    win32_register_api("GDI32.DLL", "GetLayout", 0, gdi_GetLayout, 1);
    win32_register_api("GDI32.DLL", "SetLayout", 0, gdi_SetLayout, 2);
    win32_register_api("GDI32.DLL", "LineTo", 0, gdi_LineTo, 3);
    win32_register_api("GDI32.DLL", "MoveToEx", 0, gdi_MoveToEx, 4);
    win32_register_api("GDI32.DLL", "SetDIBitsToDevice", 0, gdi_SetDIBitsToDevice, 12);

    win32_register_api("GDI32.DLL", "SetMapMode", 0, gdi_dummy_success, 2);
    win32_register_api("GDI32.DLL", "SetViewportExtEx", 0, gdi_dummy_success, 4);
    win32_register_api("GDI32.DLL", "SetWindowExtEx", 0, gdi_dummy_success, 4);
    win32_register_api("GDI32.DLL", "LPtoDP", 0, gdi_dummy_success, 3);
    win32_register_api("GDI32.DLL", "AbortDoc", 0, gdi_dummy_success, 1);
    win32_register_api("GDI32.DLL", "EndDoc", 0, gdi_dummy_success, 1);
    win32_register_api("GDI32.DLL", "SetAbortProc", 0, gdi_dummy_success, 2);
    win32_register_api("GDI32.DLL", "StartDocW", 0, gdi_dummy_success, 2);
    win32_register_api("GDI32.DLL", "StartPage", 0, gdi_dummy_success, 1);
    win32_register_api("GDI32.DLL", "EndPage", 0, gdi_dummy_success, 1);
    win32_register_api("GDI32.DLL", "CreateDCW", 0, gdi_CreateCompatibleDC, 4);
    win32_register_api("GDI32.DLL", "EnumFontsW", 0, gdi_dummy_success, 4);
}
