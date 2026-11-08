#ifndef VKEYBOARD_H
#define VKEYBOARD_H

#include "../common.h"

void vkeyboard_init(void);
void vkeyboard_toggle(void);
bool vkeyboard_is_visible(void);
void vkeyboard_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride);
bool vkeyboard_handle_tap(int x, int y);

#endif /* VKEYBOARD_H */
