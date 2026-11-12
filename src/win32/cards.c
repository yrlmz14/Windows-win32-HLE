#include "cards.h"
#include "gdi32.h"
#include "win32_dispatch.h"
#include <stdio.h>
#include <string.h>

static uint32_t cards_cdtInit(win32_process_t *proc, uint32_t *args) {
    uint32_t pdx = args[0];
    uint32_t pdy = args[1];
    int dx = 71;
    int dy = 96;
    if (pdx) guest_mem_write(&proc->mem, pdx, &dx, 4);
    if (pdy) guest_mem_write(&proc->mem, pdy, &dy, 4);
    LOGI("CARDS!cdtInit -> dx=%d, dy=%d", dx, dy);
    return 1; /* TRUE */
}

static uint32_t cards_cdtTerm(win32_process_t *proc, uint32_t *args) {
    (void)proc; (void)args;
    LOGI("CARDS!cdtTerm");
    return 1; /* TRUE */
}

/* Card drawing helper */
static void draw_card_surface(gdi_object_t *bm, int cx, int cy, int cw, int ch, int card, int mode) {
    if (!bm || !bm->bitmap.bits || cw <= 0 || ch <= 0) return;
    uint8_t *fb = bm->bitmap.bits;
    int stride = bm->bitmap.stride;
    int bw = bm->bitmap.width;
    int bh = bm->bitmap.height;

    /* Mode 3 (mdGhost) or Mode 4 (mdOGhost) = empty slot outline (tableau/foundation placeholder) */
    if (mode == 3 || mode == 4 || card == 0xFF) {
        for (int y = 0; y < ch; y++) {
            int py = cy + y;
            if (py < 0 || py >= bh) continue;
            for (int x = 0; x < cw; x++) {
                int px = cx + x;
                if (px < 0 || px >= bw) continue;
                bool is_edge = (x == 0 || x == cw - 1 || y == 0 || y == ch - 1);
                if (is_edge) {
                    *(uint32_t *)(fb + py * stride + px * 4) = 0xFF004000; /* Dark green border */
                }
            }
        }
        return;
    }

    /* Mode 1 or card >= 52 = card back */
    bool is_back = (mode == 1 || card >= 52);

    /* Draw card body */
    for (int y = 0; y < ch; y++) {
        int py = cy + y;
        if (py < 0 || py >= bh) continue;
        for (int x = 0; x < cw; x++) {
            int px = cx + x;
            if (px < 0 || px >= bw) continue;

            /* Rounded corners: skip corner pixels */
            if ((x == 0 && (y == 0 || y == ch - 1)) ||
                (x == cw - 1 && (y == 0 || y == ch - 1))) {
                continue;
            }

            bool is_border = (x == 0 || x == cw - 1 || y == 0 || y == ch - 1);
            if (is_border) {
                *(uint32_t *)(fb + py * stride + px * 4) = 0xFF000000; /* Black 1px border */
            } else if (is_back) {
                /* Classic blue crosshatch card back */
                bool inner_border = (x <= 3 || x >= cw - 4 || y <= 3 || y >= ch - 4);
                if (inner_border) {
                    *(uint32_t *)(fb + py * stride + px * 4) = 0xFFFFFFFF;
                } else {
                    bool hatch = ((x + y) % 6 < 3) ^ ((x - y + 1000) % 6 < 3);
                    *(uint32_t *)(fb + py * stride + px * 4) = hatch ? 0xFF000080 : 0xFF0000FF;
                }
            } else {
                *(uint32_t *)(fb + py * stride + px * 4) = 0xFFFFFFFF; /* White face */
            }
        }
    }

    if (is_back) return;

    /* Card Face text and symbols */
    int suit = card % 4;        /* 0 = Club, 1 = Diamond, 2 = Heart, 3 = Spade */
    int rank = card / 4;        /* 0 = Ace, 1..9 = 2..10, 10 = J, 11 = Q, 12 = K */
    static const char *s_ranks[] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    static const char s_suits[] = { 'C', 'D', 'H', 'S' };

    uint32_t text_color = (suit == 1 || suit == 2) ? 0xFFCC0000 : 0xFF000000; /* Red for D/H, Black for C/S */
    const char *r_str = (rank >= 0 && rank < 13) ? s_ranks[rank] : "?";
    char suit_ch = (suit >= 0 && suit < 4) ? s_suits[suit] : '?';

    /* Top-left rank & suit */
    for (int i = 0; r_str[i]; i++) {
        char ch = r_str[i];
        if (ch >= 32 && ch <= 126) {
            const uint8_t *glyph = GDI_FONT_8X8[ch - 32];
            for (int gy = 0; gy < 8; gy++) {
                int py = cy + 4 + gy;
                if (py < 0 || py >= bh) continue;
                for (int gx = 0; gx < 8; gx++) {
                    int px = cx + 4 + (i * 8) + gx;
                    if (px < 0 || px >= bw) continue;
                    if (glyph[gy] & (0x80 >> gx)) {
                        *(uint32_t *)(fb + py * stride + px * 4) = text_color;
                    }
                }
            }
        }
    }

    /* Top-left suit below rank */
    {
        const uint8_t *glyph = GDI_FONT_8X8[suit_ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = cy + 14 + gy;
            if (py < 0 || py >= bh) continue;
            for (int gx = 0; gx < 8; gx++) {
                int px = cx + 4 + gx;
                if (px < 0 || px >= bw) continue;
                if (glyph[gy] & (0x80 >> gx)) {
                    *(uint32_t *)(fb + py * stride + px * 4) = text_color;
                }
            }
        }
    }

    /* Center suit symbol */
    {
        int mid_x = cx + (cw - 8) / 2;
        int mid_y = cy + (ch - 8) / 2;
        const uint8_t *glyph = GDI_FONT_8X8[suit_ch - 32];
        for (int gy = 0; gy < 8; gy++) {
            int py = mid_y + gy;
            if (py < 0 || py >= bh) continue;
            for (int gx = 0; gx < 8; gx++) {
                int px = mid_x + gx;
                if (px < 0 || px >= bw) continue;
                if (glyph[gy] & (0x80 >> gx)) {
                    *(uint32_t *)(fb + py * stride + px * 4) = text_color;
                }
            }
        }
    }
}

static uint32_t cards_cdtDraw(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x     = (int)args[1];
    int y     = (int)args[2];
    int card  = (int)args[3];
    int mode  = (int)args[4];
    /* uint32_t color = args[5]; */

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm) return 0;

    draw_card_surface(bm, x, y, 71, 96, card, mode);
    return 1;
}

static uint32_t cards_cdtDrawExt(win32_process_t *proc, uint32_t *args) {
    (void)proc;
    H_HDC hdc = (H_HDC)args[0];
    int x     = (int)args[1];
    int y     = (int)args[2];
    int dx    = (int)args[3];
    int dy    = (int)args[4];
    int card  = (int)args[5];
    int mode  = (int)args[6];
    /* uint32_t color = args[7]; */

    gdi_dc_t *dc = gdi_get_dc(hdc);
    if (!dc) return 0;
    gdi_object_t *bm = gdi_get_obj((H_HANDLE)dc->selected_bitmap);
    if (!bm) return 0;

    draw_card_surface(bm, x, y, dx > 0 ? dx : 71, dy > 0 ? dy : 96, card, mode);
    return 1;
}

void cards_init(void) {
    win32_register_api("CARDS.dll", "cdtInit", 0, cards_cdtInit, 2);
    win32_register_api("CARDS.dll", "cdtTerm", 0, cards_cdtTerm, 0);
    win32_register_api("CARDS.dll", "cdtDraw", 0, cards_cdtDraw, 6);
    win32_register_api("CARDS.dll", "cdtDrawExt", 0, cards_cdtDrawExt, 8);
    win32_register_api("CARDS.DLL", "cdtInit", 0, cards_cdtInit, 2);
    win32_register_api("CARDS.DLL", "cdtTerm", 0, cards_cdtTerm, 0);
    win32_register_api("CARDS.DLL", "cdtDraw", 0, cards_cdtDraw, 6);
    win32_register_api("CARDS.DLL", "cdtDrawExt", 0, cards_cdtDrawExt, 8);
}
