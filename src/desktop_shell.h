#ifndef DESKTOP_SHELL_H
#define DESKTOP_SHELL_H

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

#define TASKBAR_HEIGHT 40

void desktop_shell_init(void);
void desktop_shell_step_anim(void);
void desktop_shell_draw(uint8_t *canvas, int canvas_w, int canvas_h, int stride);
bool desktop_shell_handle_tap(int x, int y);

bool desktop_shell_is_menu_open(void);
void desktop_shell_set_menu_open(bool open);
void desktop_shell_toggle_menu(void);

bool desktop_shell_is_start_open(void);
void desktop_shell_toggle_start(void);

#endif /* DESKTOP_SHELL_H */
