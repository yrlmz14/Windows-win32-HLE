#ifndef KINDLE_TOUCH_H
#define KINDLE_TOUCH_H

#include "../common.h"

int  touch_init(void);
void touch_cleanup(void);
void touch_poll(void);

bool touch_take_dirty(void);
bool touch_is_right_click_mode(void);
void touch_set_right_click_mode(bool on);

#endif /* KINDLE_TOUCH_H */
