#ifndef NAVIDROMEPAGE_H
#define NAVIDROMEPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Navidrome category menu, opened from Streaming.
extern lv_obj_t *navidrome_screen;

// Reserved screen flag: marks page-family membership without retaining widgets.
static inline void navidromepage_register(lv_obj_t *screen) {
	lv_obj_set_user_flag(screen, 3, true);
}
void navidromepage_init(gui_config_t *cfg);

#endif /* NAVIDROMEPAGE_H */
