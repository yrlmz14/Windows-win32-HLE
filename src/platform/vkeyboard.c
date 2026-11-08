#include "vkeyboard.h"
#include "eink_render.h"
#include "../win32/user32.h"

static bool s_vk_visible = false;
static bool s_shift = false;
static bool s_caps = false;

#define VK_HEIGHT 300
#define VK_Y (DEFAULT_SCREEN_H - VK_HEIGHT)

typedef struct {
    int x, y, w, h;
    char normal;
    char shifted;
    const char *label;
    int vkey;
} vk_key_t;

#define MAX_VK_KEYS 64
static vk_key_t s_keys[MAX_VK_KEYS];
static int s_key_count = 0;

static void add_key(int x, int y, int w, int h, char norm, char sh, const char *lbl, int vk) {
    if (s_key_count >= MAX_VK_KEYS) return;
    s_keys[s_key_count].x = x;
    s_keys[s_key_count].y = y;
    s_keys[s_key_count].w = w;
    s_keys[s_key_count].h = h;
    s_keys[s_key_count].normal = norm;
    s_keys[s_key_count].shifted = sh;
    s_keys[s_key_count].label = lbl;
    s_keys[s_key_count].vkey = vk;
    s_key_count++;
}

void vkeyboard_init(void) {
    s_key_count = 0;
    int row_h = 56;
    int pad = 4;
    int start_y = VK_Y + 10;

    /* Row 1: Numbers */
    const char *nums = "1234567890-=";
    const char *syms = "!@#$%^&*()_+";
    int kw = (DEFAULT_SCREEN_W - 20) / 14;
    for (int i = 0; i < 12; i++) {
        char lbl[2] = {nums[i], 0};
        add_key(10 + i * kw, start_y, kw - pad, row_h - pad, nums[i], syms[i], strdup(lbl), '0' + i);
    }
    add_key(10 + 12 * kw, start_y, kw * 2 - pad, row_h - pad, '\b', '\b', "Bksp", 8);

    /* Row 2: QWERTY */
    start_y += row_h;
    const char *row2 = "qwertyuiop[]";
    const char *row2_sh = "QWERTYUIOP{}";
    add_key(10, start_y, (int)(kw * 1.5f) - pad, row_h - pad, '\t', '\t', "Tab", 9);
    for (int i = 0; i < 12; i++) {
        char lbl[2] = {row2[i] - 32, 0};
        add_key(10 + (int)(kw * 1.5f) + i * kw, start_y, kw - pad, row_h - pad, row2[i], row2_sh[i], strdup(lbl), row2[i] - 32);
    }

    /* Row 3: ASDF */
    start_y += row_h;
    const char *row3 = "asdfghjkl;'";
    const char *row3_sh = "ASDFGHJKL:\"";
    add_key(10, start_y, (int)(kw * 1.8f) - pad, row_h - pad, 0, 0, "Caps", 20);
    for (int i = 0; i < 11; i++) {
        char lbl[2] = {row3[i] >= 'a' && row3[i] <= 'z' ? row3[i] - 32 : row3[i], 0};
        add_key(10 + (int)(kw * 1.8f) + i * kw, start_y, kw - pad, row_h - pad, row3[i], row3_sh[i], strdup(lbl), lbl[0]);
    }
    add_key(10 + (int)(kw * 1.8f) + 11 * kw, start_y, DEFAULT_SCREEN_W - 20 - (int)(kw * 12.8f), row_h - pad, '\n', '\n', "Enter", 13);

    /* Row 4: ZXCV */
    start_y += row_h;
    const char *row4 = "zxcvbnm,./";
    const char *row4_sh = "ZXCVBNM<>?";
    add_key(10, start_y, (int)(kw * 2.2f) - pad, row_h - pad, 0, 0, "Shift", 16);
    for (int i = 0; i < 10; i++) {
        char lbl[2] = {row4[i] >= 'a' && row4[i] <= 'z' ? row4[i] - 32 : row4[i], 0};
        add_key(10 + (int)(kw * 2.2f) + i * kw, start_y, kw - pad, row_h - pad, row4[i], row4_sh[i], strdup(lbl), lbl[0]);
    }

    /* Row 5: Space, Esc, Close */
    start_y += row_h;
    add_key(10, start_y, kw * 2 - pad, row_h - pad, 27, 27, "Esc", 27);
    add_key(10 + kw * 2, start_y, kw * 8 - pad, row_h - pad, ' ', ' ', "Space", 32);
    add_key(10 + kw * 10, start_y, kw * 4 - pad, row_h - pad, 0, 0, "Hide Keyboard", 0);
}

void vkeyboard_toggle(void) {
    s_vk_visible = !s_vk_visible;
}

bool vkeyboard_is_visible(void) {
    return s_vk_visible;
}

void vkeyboard_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride) {
    (void)canvas_w; (void)canvas_h; (void)stride;
    if (!s_vk_visible) return;

    /* Background panel */
    eink_fill_rect(0, VK_Y, DEFAULT_SCREEN_W, VK_HEIGHT, EINK_LIGHT_GRAY);
    eink_fill_rect(0, VK_Y, DEFAULT_SCREEN_W, 3, EINK_BLACK);

    /* Draw each key */
    for (int i = 0; i < s_key_count; i++) {
        vk_key_t *k = &s_keys[i];
        eink_fill_rect(k->x, k->y, k->w, k->h, EINK_WHITE);
        eink_draw_rect(k->x, k->y, k->w, k->h, EINK_BLACK, 2);

        /* Draw key label */
        const char *txt = k->label;
        if (k->normal >= 'a' && k->normal <= 'z') {
            txt = (s_shift ^ s_caps) ? &k->shifted : &k->normal;
        }
        eink_draw_string(k->x + (k->w / 2) - 8, k->y + (k->h / 2) - 4, txt, EINK_BLACK);
    }
}

bool vkeyboard_handle_tap(int x, int y) {
    if (!s_vk_visible || y < VK_Y) return false;

    for (int i = 0; i < s_key_count; i++) {
        vk_key_t *k = &s_keys[i];
        if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h) {
            if (strcmp(k->label, "Hide Keyboard") == 0) {
                s_vk_visible = false;
                return true;
            }
            if (strcmp(k->label, "Shift") == 0) {
                s_shift = !s_shift;
                return true;
            }
            if (strcmp(k->label, "Caps") == 0) {
                s_caps = !s_caps;
                return true;
            }

            char ch = (s_shift ^ s_caps) ? k->shifted : k->normal;
            user_inject_key(k->vkey, ch, true);
            user_inject_key(k->vkey, ch, false);
            if (s_shift) s_shift = false;
            return true;
        }
    }
    return true; /* Consumed inside keyboard area */
}
